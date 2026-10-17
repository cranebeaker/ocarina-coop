// Story Co-op: shared progression for a party.
//
// The client captures a compact snapshot of the party-wide progress (StoryProgress), uploads it when it changes and
// merges the snapshot the server sends back into its own save. Anything that is per player (rupees, health, ammo,
// bottle contents, button items) is never part of the snapshot. Everything here is gated on StoryCoopActive(), so with
// the party setting off nothing in this file runs.

#include "ZeldaOnlineClient.hpp"

#include "soh/OTRGlobals.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/item-tables/ItemTableManager.h"
#include "soh/Enhancements/randomizer/randomizer.h"
#include "soh/Notification/Notification.h"
#include "soh/Enhancements/SwitchAge.h"
#include "Packet.hpp"
#include "PacketTypes.hpp"
#include "StoryProgress.hpp"

#include <cstring>

extern "C" {
#include "variables.h"
#include "z64.h"
#include "functions.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
// z_player.c: puts Link back into his normal standing action
void func_80839F90(Player* player, PlayState* play);
// z_player.c: the action Link waits in after taking the sword off the pedestal, until the cutscene takes over
void Player_Action_8084F608(Player* player, PlayState* play);
// z_player.c: clears the carry state, resets camera and sub states, and lets go of any held actor
void func_80832564(PlayState* play, Player* player);
void Player_UseItem(PlayState* play, Player* player, s32 item);
}

namespace {

using namespace ZeldaOnline;

// The pedestal grab leaves Link frozen in a cutscene-like action until the sequence starts (the action waits for the
// cutscene, the carry flag is set and the cutscene sword may be in his hand). When the vote is cancelled or fails he
// has to be handed back to normal control. Other state can have changed while he waited (a hit replaces the action and
// clears the cutscene flag but not the carry flag), so every piece is checked on its own instead of relying on one flag.
void StoryRestorePlayer() {
    PlayState* play = gPlayState;
    if (play == nullptr)
        return;

    Player* player = GET_PLAYER(play);
    if (player == nullptr || play->csCtx.state != CS_STATE_IDLE)
        return;

    // never interfere with a real item get or cutscene action
    if (player->stateFlags1 & (PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_IN_ITEM_CS))
        return;

    bool waitingAction = player->actionFunc == Player_Action_8084F608;
    bool staleCarry = (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) && player->heldActor == nullptr;
    bool frozen = waitingAction || (player->stateFlags1 & PLAYER_STATE1_IN_CUTSCENE);
    if (!frozen && !staleCarry)
        return;

    // carry state, camera and sub states (also lets go of a held actor, if any)
    func_80832564(play, player);
    if (player->heldActor == nullptr) {
        player->stateFlags1 &= ~PLAYER_STATE1_CARRYING_ACTOR;
        player->interactRangeActor = nullptr;
    }

    // the adult placement path swaps the equipped sword for the cutscene sword: sheathe it again
    if (player->heldItemAction == PLAYER_IA_SWORD_CS || player->itemAction == PLAYER_IA_SWORD_CS)
        Player_UseItem(play, player, ITEM_NONE);

    // control and animation: back to the standing action (this also clears the cutscene flag)
    if (frozen) {
        player->stateFlags1 &= ~PLAYER_STATE1_IN_CUTSCENE;
        func_80839F90(player, play);
    }
}

const int STORY_SLOT_BEAN = SLOT_BEAN;

bool StoryIsPartyScene(int sceneNum) {
    switch (sceneNum) {
        case SCENE_DEKU_TREE:
        case SCENE_DODONGOS_CAVERN:
        case SCENE_JABU_JABU:
        case SCENE_FOREST_TEMPLE:
        case SCENE_FIRE_TEMPLE:
        case SCENE_WATER_TEMPLE:
        case SCENE_SPIRIT_TEMPLE:
        case SCENE_SHADOW_TEMPLE:
        case SCENE_BOTTOM_OF_THE_WELL:
        case SCENE_ICE_CAVERN:
        case SCENE_THIEVES_HIDEOUT:
        case SCENE_INSIDE_GANONS_CASTLE:
        case SCENE_DEKU_TREE_BOSS:
        case SCENE_DODONGOS_CAVERN_BOSS:
        case SCENE_JABU_JABU_BOSS:
        case SCENE_FOREST_TEMPLE_BOSS:
        case SCENE_FIRE_TEMPLE_BOSS:
        case SCENE_WATER_TEMPLE_BOSS:
        case SCENE_SPIRIT_TEMPLE_BOSS:
        case SCENE_SHADOW_TEMPLE_BOSS:
        case SCENE_GANONDORF_BOSS:
        case SCENE_GANON_BOSS:
            return true;
        default:
            return false;
    }
}

bool StoryIsBottleSlot(int slot) {
    return slot >= SLOT_BOTTLE_1 && slot <= SLOT_BOTTLE_4;
}

// Slots that are never shared as slot items (bottles and adult trade items are per player, beans are bought).
// The child trade slot is shared up to Zelda's Letter (see StoryTradeChildItem); the mask chain after it stays per
// player. The randomizer shuffles these items, so there the slot stays per player.
bool StorySlotIsPerPlayer(int slot) {
    return StoryIsBottleSlot(slot) || slot == SLOT_TRADE_ADULT || (slot == SLOT_TRADE_CHILD && IS_RANDO) ||
           slot == STORY_SLOT_BEAN;
}

// Child trade item that is shared: Weird Egg, Chicken or Zelda's Letter. Whoever is further along in the mask chain
// counts as having the letter. The letter only travels once Zelda's Lullaby does, because Sram_OpenSave takes the
// letter back from a save that has the courtyard flag but no Lullaby.
u8 StoryTradeChildItem(u8 item, bool lullaby) {
    if (item < ITEM_WEIRD_EGG || item > ITEM_SOLD_OUT)
        return ITEM_NONE;
    if (item >= ITEM_LETTER_ZELDA)
        return lullaby ? (u8)ITEM_LETTER_ZELDA : (u8)ITEM_CHICKEN;
    return item;
}

// eventChkInf and infTable masks of the bits that must never be shared
// lullaby: Zelda's Lullaby is owned, which lets the courtyard flag (Zelda's Letter) travel
void StoryEventChkInfMask(uint16_t out[14], bool lullaby) {
    for (int i = 0; i < 14; i++)
        out[i] = 0xFFFF;
    if (IS_RANDO || !lullaby) {
        // 0x12 weird egg from Malon (randomizer only, the item is shuffled there) and 0x40 Zelda's Letter
        // (without Lullaby the next load undoes it)
        if (IS_RANDO)
            out[0x12 >> 4] &= (uint16_t)~(1 << (0x12 & 0xF));
        out[0x40 >> 4] &= (uint16_t)~(1 << (0x40 & 0xF));
    }
    for (int flag : STORY_EVENTCHKINF_EXCLUDE) {
        if ((flag >> 4) < 14)
            out[flag >> 4] &= (uint16_t)~(1 << (flag & 0xF));
    }
    StoryClearFlagBits(out, 14, STORY_EVENTCHKINF_ITEM_EXCLUDE,
                       (int)(sizeof(STORY_EVENTCHKINF_ITEM_EXCLUDE) / sizeof(int)));
}

// itemGetInf and infTable masks of the "NPC already gave me item X" bits that stay per player
void StoryItemGetInfMask(uint16_t out[4]) {
    for (int i = 0; i < 4; i++)
        out[i] = 0xFFFF;
    StoryClearFlagBits(out, 4, STORY_ITEMGETINF_EXCLUDE, (int)(sizeof(STORY_ITEMGETINF_EXCLUDE) / sizeof(int)));
}

void StoryInfTableMask(uint16_t out[30]) {
    for (int i = 0; i < 30; i++)
        out[i] = (i == STORY_INFTABLE_EXCLUDE_INDEX) ? 0 : 0xFFFF;
    StoryClearFlagBits(out, 30, STORY_INFTABLE_ITEM_EXCLUDE,
                       (int)(sizeof(STORY_INFTABLE_ITEM_EXCLUDE) / sizeof(int)));
}

int StoryUpgradeValue(u32 upgrades, int type) {
    return (int)((upgrades & STORY_UPGRADE_MASKS[type]) >> STORY_UPGRADE_SHIFTS[type]);
}

// Item given to reach an upgrade level, or ITEM_NONE when the level is set directly
u8 StoryUpgradeItem(int type, int level) {
    static const u8 kUpgradeItems[8][4] = {
        { ITEM_NONE, ITEM_QUIVER_30, ITEM_QUIVER_40, ITEM_QUIVER_50 },           // UPG_QUIVER
        { ITEM_NONE, ITEM_BOMB_BAG_20, ITEM_BOMB_BAG_30, ITEM_BOMB_BAG_40 },     // UPG_BOMB_BAG
        { ITEM_NONE, ITEM_BRACELET, ITEM_GAUNTLETS_SILVER, ITEM_GAUNTLETS_GOLD }, // UPG_STRENGTH
        { ITEM_NONE, ITEM_SCALE_SILVER, ITEM_SCALE_GOLDEN, ITEM_NONE },          // UPG_SCALE
        { ITEM_NONE, ITEM_WALLET_ADULT, ITEM_WALLET_GIANT, ITEM_NONE },          // UPG_WALLET
        { ITEM_NONE, ITEM_NONE, ITEM_BULLET_BAG_40, ITEM_BULLET_BAG_50 },        // UPG_BULLET_BAG
        { ITEM_NONE, ITEM_NONE, ITEM_STICK_UPGRADE_20, ITEM_STICK_UPGRADE_30 },  // UPG_STICKS
        { ITEM_NONE, ITEM_NONE, ITEM_NUT_UPGRADE_30, ITEM_NUT_UPGRADE_40 },      // UPG_NUTS
    };
    if (type < 0 || type >= 8 || level < 0 || level > 3)
        return ITEM_NONE;
    return kUpgradeItems[type][level];
}

// Quest item bit -> item id for the bits Item_Give understands (songs, medallions, stones, cards)
bool StoryQuestBitToItem(int bit, u8& item) {
    if (bit >= QUEST_MEDALLION_FOREST && bit <= QUEST_MEDALLION_LIGHT) {
        item = (u8)(ITEM_MEDALLION_FOREST + (bit - QUEST_MEDALLION_FOREST));
        return true;
    }
    if (bit >= QUEST_SONG_MINUET && bit <= QUEST_SONG_STORMS) {
        item = (u8)(ITEM_SONG_MINUET + (bit - QUEST_SONG_MINUET));
        return true;
    }
    if (bit >= QUEST_KOKIRI_EMERALD && bit <= QUEST_ZORA_SAPPHIRE) {
        item = (u8)(ITEM_KOKIRI_EMERALD + (bit - QUEST_KOKIRI_EMERALD));
        return true;
    }
    if (bit == QUEST_STONE_OF_AGONY) {
        item = ITEM_STONE_OF_AGONY;
        return true;
    }
    if (bit == QUEST_GERUDO_CARD) {
        item = ITEM_GERUDO_CARD;
        return true;
    }
    return false;
}

int StoryPopCount(u32 v) {
    int n = 0;
    for (; v != 0; v &= v - 1)
        n++;
    return n;
}

} // namespace

bool ZeldaOnlineClient::StoryCoopActive() const {
    return isConnected && m_partyID != 0 && m_partySettings.storyCoop;
}

bool ZeldaOnlineClient::StoryShareConsumables() const {
    return StoryCoopActive() && m_partySettings.shareAmmoRupeesHp;
}

// Called whenever the party id or the party settings may have changed.
void ZeldaOnlineClient::StoryRefreshState() {
    bool active = StoryCoopActive();

    // leaving the party or switching Story Co-op off never leaves anybody on hold
    if (!active || m_storyPartyID != m_partyID) {
        StoryReleaseHold(true);
        m_storyApprovedFrames = 0;
        m_storyMoveEra = -1;
    }

    if (active && (!m_storyWasActive || m_storyPartyID != m_partyID)) {
        // newly joined, created or switched on: upload what we have so the team save is (re)seeded.
        // Anything pending or sent for a previous party is stale, so the catch-up starts from a clean slate.
        m_storySeedPending = true;
        m_storyHasPending = false;
        m_storyPending = StoryProgress();
        m_storyLastSent = StoryProgress();
    } else if (!active) {
        m_storySeedPending = false;
        m_storyDirty = false;
        m_storyHasPending = false;
        m_storyPending = StoryProgress();
        m_storyLastSent = StoryProgress();
    }

    m_storyWasActive = active;
    m_storyPartyID = active ? m_partyID : 0U;
}

// A save was loaded or exited (title screen, file select, quit). Whatever was pending or sent belongs to the previous
// save, so it is dropped. After a load the new save is uploaded again, which also makes the server send back what
// the team has that this save lacks.
void ZeldaOnlineClient::StoryOnSaveChanged(bool loaded) {
    m_storySaveSerial++;

    m_storyHasPending = false;
    m_storyPending = StoryProgress();
    m_storyLastSent = StoryProgress();
    m_storyDirty = false;
    m_storySeedPending = false;

    // dungeon progress belongs to the save that is gone, the next save gets it back from the team save
    std::memset(m_storyDungeonSwch, 0, sizeof(m_storyDungeonSwch));
    std::memset(m_storyDungeonClear, 0, sizeof(m_storyDungeonClear));

    // a vote belongs to the scene of the save that is gone
    if (m_storyHold) {
        StorySendVote(false);
        m_storyHold = false;
        m_storyMissing.clear();
    }
    m_storyApprovedFrames = 0;

    if (loaded && StoryCoopActive())
        m_storySeedPending = true;
}

void ZeldaOnlineClient::StoryMarkDirty() {
    if (StoryCoopActive())
        m_storyDirty = true;
}

// Called when a dungeon or boss scene is entered, right before the saved switch and clear flags are emptied for the
// server instance. They are what the player had when the scene was last left, like the vanilla save keeps them.
void ZeldaOnlineClient::StoryRememberDungeonFlags(int sceneNum, uint32_t swch, uint32_t clear) {
    if (!StoryCoopActive() || sceneNum < 0 || sceneNum >= STORY_SCENE_COUNT)
        return;

    m_storyDungeonSwch[sceneNum] |= swch & ~StorySceneSwchExclude(sceneNum);
    m_storyDungeonClear[sceneNum] |= clear;
}

// Called once the server instance flags of a dungeon or boss scene have been adopted. The party's permanent flags are
// added on top, so a dungeon whose instance was reset still has its doors open and its rooms cleared.
void ZeldaOnlineClient::StoryApplyDungeonFlags(PlayState* play) {
    if (!StoryCoopActive() || play == nullptr || play->sceneNum < 0 || play->sceneNum >= STORY_SCENE_COUNT)
        return;

    play->actorCtx.flags.swch |= m_storyDungeonSwch[play->sceneNum] & ~StorySceneSwchExclude(play->sceneNum);
    play->actorCtx.flags.clear |= m_storyDungeonClear[play->sceneNum];
}

StoryProgress ZeldaOnlineClient::StoryCapture() {
    StoryProgress p;

    Inventory& inv = gSaveContext.inventory;

    int bottles = 0;
    bool hasLetter = false;
    for (int i = 0; i < 24; i++) {
        if (StoryIsBottleSlot(i)) {
            if (inv.items[i] != ITEM_NONE) {
                bottles++;
                if (inv.items[i] == ITEM_LETTER_RUTO)
                    hasLetter = true;
            }
            p.items[i] = ITEM_NONE;
        } else if (StorySlotIsPerPlayer(i)) {
            p.items[i] = ITEM_NONE;
        } else if (i == SLOT_TRADE_CHILD) {
            p.items[i] = StoryTradeChildItem(inv.items[i], CHECK_QUEST_ITEM(QUEST_SONG_LULLABY) != 0);
        } else {
            p.items[i] = inv.items[i];
        }
    }
    p.bottleCount = (uint8_t)bottles;
    p.hasRutoLetter = hasLetter ? 1 : 0;

    p.equipment = (uint16_t)(inv.equipment & ~STORY_EQUIPMENT_EXCLUDE);
    p.upgrades = inv.upgrades;
    p.questItems = inv.questItems & ~STORY_QUEST_ITEMS_EXCLUDE;
    std::memcpy(p.dungeonItems, inv.dungeonItems, sizeof(p.dungeonItems));
    std::memcpy(p.dungeonKeys, inv.dungeonKeys, sizeof(p.dungeonKeys));

    int pieces = (int)((inv.questItems & 0xF0000000u) >> 28);
    int quarters = (gSaveContext.healthCapacity / 0x10) * 4 + (pieces & 3);
    if (quarters < 0)
        quarters = 0;
    if (quarters > 80)
        quarters = 80;
    p.heartQuarters = (uint8_t)quarters;

    p.isMagicAcquired = gSaveContext.isMagicAcquired ? 1 : 0;
    p.isDoubleMagicAcquired = gSaveContext.isDoubleMagicAcquired ? 1 : 0;
    p.magicLevel = (uint8_t)(p.isMagicAcquired + p.isDoubleMagicAcquired);
    p.isDoubleDefenseAcquired = gSaveContext.isDoubleDefenseAcquired ? 1 : 0;
    p.defenseHearts = inv.defenseHearts > 0 ? (uint8_t)inv.defenseHearts : 0;
    p.bgsFlag = gSaveContext.bgsFlag ? 1 : 0;
    p.swordHealth = gSaveContext.swordHealth;
    p.gsTokens = inv.gsTokens > 0 ? (uint16_t)inv.gsTokens : 0;

    uint16_t eventMask[14];
    StoryEventChkInfMask(eventMask, CHECK_QUEST_ITEM(QUEST_SONG_LULLABY) != 0);
    for (int i = 0; i < 14; i++)
        p.eventChkInf[i] = gSaveContext.eventChkInf[i] & eventMask[i];
    uint16_t itemGetMask[4];
    StoryItemGetInfMask(itemGetMask);
    for (int i = 0; i < 4; i++)
        p.itemGetInf[i] = gSaveContext.itemGetInf[i] & itemGetMask[i];
    uint16_t infTableMask[30];
    StoryInfTableMask(infTableMask);
    for (int i = 0; i < 30; i++)
        p.infTable[i] = gSaveContext.infTable[i] & infTableMask[i];
    // eventInf holds per player state (Lon Lon Ranch race, horse state): only the reviewed bits in the share mask travel
    for (int i = 0; i < 4; i++)
        p.eventInf[i] = gSaveContext.eventInf[i] & STORY_EVENTINF_SHARE_MASK[i];
    for (int i = 0; i < 6; i++)
        p.gsFlags[i] = gSaveContext.gsFlags[i];

    if (IS_RANDO) {
        size_t words = sizeof(gSaveContext.ship.randomizerInf) / sizeof(gSaveContext.ship.randomizerInf[0]);
        p.randomizerInf.assign(gSaveContext.ship.randomizerInf, gSaveContext.ship.randomizerInf + words);
    }

    for (int s = 0; s < STORY_SCENE_COUNT; s++) {
        StorySceneFlags scene;
        scene.sceneNum = (uint8_t)s;

        const SavedSceneFlags& saved = gSaveContext.sceneFlags[s];
        scene.chest = saved.chest;
        scene.swch = saved.swch;
        scene.clear = saved.clear;
        scene.collect = saved.collect;

        // the loaded scene keeps its live flags in the actor context until it is saved on exit
        if (gPlayState != nullptr && gPlayState->sceneNum == s) {
            scene.chest = gPlayState->actorCtx.flags.chest;
            scene.collect = gPlayState->actorCtx.flags.collect;
            // dungeons and boss rooms keep their live switch and clear flags in the server instance while loaded
            // (the saved copy is empty then), so only what was kept at the last visit is shared
            if (!StoryIsPartyScene(s)) {
                scene.swch = gPlayState->actorCtx.flags.swch;
                scene.clear = gPlayState->actorCtx.flags.clear;
            }
        }

        // permanent dungeon progress; the Water and Forest Temple exclusions are applied by MergeScene
        if (StoryIsPartyScene(s)) {
            scene.swch |= m_storyDungeonSwch[s];
            scene.clear |= m_storyDungeonClear[s];
        }

        p.MergeScene(scene);
    }

    return p;
}

bool ZeldaOnlineClient::StoryCanApply() {
    PlayState* play = gPlayState;
    if (play == nullptr || gSaveContext.gameMode != GAMEMODE_NORMAL || gSaveContext.fileNum == 0xFF)
        return false;

    Player* player = GET_PLAYER(play);
    return player != nullptr && play->transitionTrigger == TRANS_TRIGGER_OFF && play->transitionMode == TRANS_MODE_OFF &&
           play->msgCtx.msgMode == MSGMODE_NONE && play->pauseCtx.state == 0 && play->csCtx.state == CS_STATE_IDLE &&
           play->gameOverCtx.state == GAMEOVER_INACTIVE &&
           !(player->stateFlags1 & (PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_GETTING_ITEM));
}

// Merges the team snapshot into the local save. Returns true when anything changed.
bool ZeldaOnlineClient::StoryApply(const StoryProgress& team) {
    PlayState* play = gPlayState;
    if (play == nullptr)
        return false;

    StoryProgress before = StoryCapture();
    Inventory& inv = gSaveContext.inventory;

    // Item_Give would otherwise echo every item back to the server through the OnItemReceive hooks
    bool previousSkip = m_skipItemGive;
    m_skipItemGive = true;

    // flags first, later steps look at them
    uint16_t eventMask[14];
    bool lullaby = CHECK_QUEST_ITEM(QUEST_SONG_LULLABY) || (team.questItems & (1u << QUEST_SONG_LULLABY));
    StoryEventChkInfMask(eventMask, lullaby);
    for (int i = 0; i < 14; i++)
        gSaveContext.eventChkInf[i] |= (team.eventChkInf[i] & eventMask[i]);
    uint16_t itemGetMask[4];
    StoryItemGetInfMask(itemGetMask);
    for (int i = 0; i < 4; i++)
        gSaveContext.itemGetInf[i] |= (team.itemGetInf[i] & itemGetMask[i]);
    uint16_t infTableMask[30];
    StoryInfTableMask(infTableMask);
    for (int i = 0; i < 30; i++)
        gSaveContext.infTable[i] |= (team.infTable[i] & infTableMask[i]);
    for (int i = 0; i < 4; i++)
        gSaveContext.eventInf[i] |= (team.eventInf[i] & STORY_EVENTINF_SHARE_MASK[i]);
    for (int i = 0; i < 6; i++)
        gSaveContext.gsFlags[i] |= team.gsFlags[i];

    if (IS_RANDO) {
        size_t words = sizeof(gSaveContext.ship.randomizerInf) / sizeof(gSaveContext.ship.randomizerInf[0]);
        for (size_t i = 0; i < team.randomizerInf.size() && i < words; i++)
            gSaveContext.ship.randomizerInf[i] |= team.randomizerInf[i];
    }

    // gold skulltulas
    int tokens = 0;
    for (int i = 0; i < 6; i++)
        tokens += StoryPopCount((u32)gSaveContext.gsFlags[i]);
    if (tokens < team.gsTokens)
        tokens = team.gsTokens;
    if (inv.gsTokens < tokens)
        inv.gsTokens = (s16)tokens;
    if (inv.gsTokens > 0)
        inv.questItems |= (1u << QUEST_SKULL_TOKEN);

    // slot items (bottles, adult trade items and beans are per player and never in the snapshot)
    for (int slot = 0; slot < 24; slot++) {
        if (StorySlotIsPerPlayer(slot))
            continue;

        u8 teamItem = team.items[slot];
        if (slot == SLOT_TRADE_CHILD)
            teamItem = StoryTradeChildItem(teamItem, lullaby);
        if (teamItem == ITEM_NONE)
            continue;

        u8 localItem = inv.items[slot];
        bool empty = localItem == ITEM_NONE;
        bool higher = !empty && teamItem > localItem;
        if (!empty && !higher)
            continue;

        // bomb and bow slots appear together with their bag or quiver below
        if (slot == SLOT_BOMB || slot == SLOT_BOW)
            continue;

        // the ammo slots (sticks, nuts, bombchus, slingshot) get their native starting ammo from Item_Give
        Item_Give(play, teamItem);
        if (inv.items[slot] == ITEM_NONE)
            inv.items[slot] = teamItem;
    }

    // upgrades; a quiver or a bomb bag also unlocks its slot, so the first level always goes through its item
    for (int type = 0; type < 8; type++) {
        int teamLevel = StoryUpgradeValue(team.upgrades, type);
        int localLevel = StoryUpgradeValue(inv.upgrades, type);
        if (teamLevel <= localLevel)
            continue;

        if (localLevel == 0 && (type == UPG_QUIVER || type == UPG_BOMB_BAG)) {
            Item_Give(play, StoryUpgradeItem(type, 1));
            localLevel = StoryUpgradeValue(inv.upgrades, type);
        }

        if (teamLevel > localLevel) {
            u8 item = StoryUpgradeItem(type, teamLevel);
            if (item != ITEM_NONE)
                Item_Give(play, item);
            else
                Inventory_ChangeUpgrade((s16)type, (s16)teamLevel);
        }
    }

    // equipment: swords, shields, tunics and boots (the Master Sword bit is excluded)
    inv.equipment |= (u16)(team.equipment & ~STORY_EQUIPMENT_EXCLUDE);
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BIGGORON)) {
        // owning the Biggoron Sword replaces the Giant's Knife
        inv.equipment &= (u16)~OWNED_EQUIP_FLAG_ALT(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BROKENGIANTKNIFE);
    }
    if (team.bgsFlag)
        gSaveContext.bgsFlag = 1;
    if (gSaveContext.swordHealth < team.swordHealth)
        gSaveContext.swordHealth = team.swordHealth;

    // quest items: songs, medallions, stones, cards
    u32 newQuest = team.questItems & ~STORY_QUEST_ITEMS_EXCLUDE & ~inv.questItems;
    for (int bit = 0; bit < 32; bit++) {
        if (!(newQuest & (1u << bit)))
            continue;
        u8 item;
        if (StoryQuestBitToItem(bit, item))
            Item_Give(play, item);
        else
            inv.questItems |= (1u << bit);
    }

    // dungeon items (boss key, compass, map)
    for (int i = 0; i < 20; i++)
        inv.dungeonItems[i] |= team.dungeonItems[i];

    // hearts: heart containers and pieces are shared as quarters of a heart
    {
        int pieces = (int)((inv.questItems & 0xF0000000u) >> 28);
        int localQuarters = (gSaveContext.healthCapacity / 0x10) * 4 + (pieces & 3);
        int target = team.heartQuarters;
        if (target > 80)
            target = 80;
        if (target > localQuarters) {
            gSaveContext.healthCapacity = (s16)(0x10 * (target / 4));
            inv.questItems = (inv.questItems & ~0xF0000000u) | ((u32)(target % 4) << 28);
            gSaveContext.healthAccumulator = 0x140;
        }
    }

    // magic and double defense
    if (team.isMagicAcquired && !gSaveContext.isMagicAcquired) {
        gSaveContext.isMagicAcquired = true;
        gSaveContext.magicFillTarget = MAGIC_NORMAL_METER;
        Magic_Fill(play);
    }
    if (team.isDoubleMagicAcquired && !gSaveContext.isDoubleMagicAcquired) {
        gSaveContext.isMagicAcquired = true;
        gSaveContext.isDoubleMagicAcquired = true;
        gSaveContext.magicFillTarget = MAGIC_DOUBLE_METER;
        gSaveContext.magicLevel = 0;
        Magic_Fill(play);
    }
    if (team.isDoubleDefenseAcquired && !gSaveContext.isDoubleDefenseAcquired) {
        gSaveContext.isDoubleDefenseAcquired = true;
        inv.defenseHearts = 20;
    }
    if (inv.defenseHearts < (s8)team.defenseHearts)
        inv.defenseHearts = (s8)team.defenseHearts;

    // bottles: the bottles themselves are shared, what is inside them stays per player
    {
        bool kingZoraMoved = Flags_GetEventChkInf(EVENTCHKINF_KING_ZORA_MOVED) != 0;
        bool hasLetter = false;
        for (int slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_4; slot++) {
            if (inv.items[slot] == ITEM_LETTER_RUTO)
                hasLetter = true;
        }

        if (kingZoraMoved && hasLetter && !IS_RANDO) {
            // the letter has been delivered, it turns into an empty bottle
            Inventory_ReplaceItem(play, ITEM_LETTER_RUTO, ITEM_BOTTLE);
            hasLetter = false;
        }

        if (team.hasRutoLetter && !hasLetter && !kingZoraMoved && !Flags_GetRandomizerInf(RAND_INF_OBTAINED_RUTOS_LETTER)) {
            bool placed = false;
            for (int slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_4 && !placed; slot++) {
                if (inv.items[slot] == ITEM_NONE) {
                    Item_Give(play, ITEM_LETTER_RUTO);
                    placed = true;
                }
            }
            for (int slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_4 && !placed; slot++) {
                if (inv.items[slot] == ITEM_BOTTLE) {
                    inv.items[slot] = ITEM_LETTER_RUTO;
                    Flags_SetRandomizerInf(RAND_INF_OBTAINED_RUTOS_LETTER);
                    placed = true;
                }
            }
        }

        int localBottles = 0;
        for (int slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_4; slot++) {
            if (inv.items[slot] != ITEM_NONE)
                localBottles++;
        }
        for (; localBottles < team.bottleCount && localBottles < 4; localBottles++)
            Item_Give(play, ITEM_BOTTLE);
    }

    // scene flags
    for (const StorySceneFlags& scene : team.scenes) {
        if (scene.sceneNum >= STORY_SCENE_COUNT)
            continue;

        bool party = StoryIsPartyScene(scene.sceneNum);
        u32 chest = scene.chest;
        u32 collect = scene.collect;
        u32 swch = scene.swch & ~StorySceneSwchExclude(scene.sceneNum);
        u32 clear = scene.clear;

        // dungeon and boss scenes: the switch and clear flags are applied the next time the scene is loaded
        // (StoryApplyDungeonFlags), the saved copy and the live flags belong to the server instance
        if (party) {
            m_storyDungeonSwch[scene.sceneNum] |= swch;
            m_storyDungeonClear[scene.sceneNum] |= clear;
            swch = 0;
            clear = 0;
        }

        SavedSceneFlags& saved = gSaveContext.sceneFlags[scene.sceneNum];
        saved.chest |= chest;
        saved.collect |= collect;
        saved.swch |= swch;
        saved.clear |= clear;

        if (play->sceneNum == scene.sceneNum) {
            play->actorCtx.flags.chest |= chest;
            play->actorCtx.flags.collect |= collect;
            play->actorCtx.flags.swch |= swch;
            play->actorCtx.flags.clear |= clear;
        }

        // dungeon instances reset to the chests the player opened, so the team chests are part of that
        if (party && chest != 0)
            m_savedChests[scene.sceneNum] |= chest;
    }

    m_skipItemGive = previousSkip;

    return StoryCapture() != before;
}

void ZeldaOnlineClient::StorySendProgress(const StoryProgress& progress, bool seed) {
    ByteStream packet = newPacket(CLIENT_PACKET_STORY_PROGRESS);
    packet << PackedUInt1(seed ? STORY_FLAG_SEED : 0);
    progress.Serialize(packet);
    WritePacket(packet);
}

// Runs every game frame while connected.
void ZeldaOnlineClient::StoryUpdate() {
    if (!StoryCoopActive() || gPlayState == nullptr)
        return;

    if (gSaveContext.gameMode != GAMEMODE_NORMAL || gSaveContext.fileNum == 0xFF)
        return;

    m_storyFrame++;
    if (m_storySendCooldown > 0)
        m_storySendCooldown--;
    if (m_storyNotifyCooldown > 0)
        m_storyNotifyCooldown--;

    // time travel vote: B withdraws, an approval that nobody consumed expires
    if (m_storyHold && (gPlayState->state.input[0].press.button & BTN_B)) {
        StorySendVote(false);
        StoryReleaseHold(true);
    }
    if (m_storyApprovedFrames > 0 && --m_storyApprovedFrames == 0)
        StoryRestorePlayer();
    StoryMoveToEra();

    // upload first, so a joining member contributes its own progress before the team save is merged into it
    bool poll = m_storyDirty || (m_storyFrame % 20) == 0;
    if ((m_storySeedPending || poll) && m_storySendCooldown == 0) {
        StoryProgress now = StoryCapture();
        if (m_storySeedPending) {
            StorySendProgress(now, true);
            m_storySeedPending = false;
            m_storyLastSent = now;
            m_storySendCooldown = 10;
        } else if (now != m_storyLastSent) {
            StorySendProgress(now, false);
            m_storyLastSent = now;
            m_storySendCooldown = 10;
        }
        m_storyDirty = false;
    }

    // never apply a snapshot that was received for another party or another save
    if (m_storyHasPending && (m_storyPendingParty != m_partyID || m_storyPendingSerial != m_storySaveSerial)) {
        m_storyHasPending = false;
        m_storyPending = StoryProgress();
    }

    if (m_storyHasPending && StoryCanApply()) {
        StoryProgress team = m_storyPending;
        m_storyHasPending = false;
        m_storyPending = StoryProgress();

        if (StoryApply(team) && m_storyNotifyCooldown == 0) {
            Notification::Emit({ .message = "Party progress received" });
            m_storyNotifyCooldown = 5 * 20;
        }
    }
}

void ZeldaOnlineClient::OnStoryState(ByteStream& packet) {
    if (packet.BytesLeft() < 1)
        return;

    auto flags = (uint8_t)packet.Read<PackedUInt1>().value();

    StoryProgress incoming;
    if (!incoming.Deserialize(packet))
        return;

    if (!StoryCoopActive())
        return;

    // No save is loaded (title screen, file select): the snapshot would land on whatever save is loaded next. The
    // upload after the load asks the server for the catch-up again.
    if (gPlayState == nullptr || gSaveContext.fileNum == 0xFF)
        return;

    // A catch-up that arrives before this save has been uploaded was answered for the previous save or for the join,
    // the upload that follows gets its own catch-up.
    if ((flags & STORY_FLAG_CATCH_UP) && m_storySeedPending)
        return;

    if (!m_storyHasPending) {
        m_storyPending = StoryProgress();
        m_storyHasPending = true;
        m_storyPendingParty = m_partyID;
        m_storyPendingSerial = m_storySaveSerial;
    }
    m_storyPending.MergeFrom(incoming);
}

// Consumable pickups, shared only with "Share Ammo, Rupees & HP"
static bool StoryIsSharedConsumable(const GetItemEntry& entry) {
    if (entry.getItemCategory != ITEM_CATEGORY_JUNK)
        return false;

    if (entry.modIndex == MOD_RANDOMIZER)
        return entry.getItemId != RG_ICE_TRAP;

    if (entry.modIndex != MOD_NONE || entry.getItemId == GI_ICE_TRAP)
        return false;

    switch (entry.itemId) {
        case ITEM_RUPEE_GOLD:
        case ITEM_BEAN:
        case ITEM_SKULL_TOKEN:
            return false;
        default:
            return true;
    }
}

void ZeldaOnlineClient::StoryOnItemReceive(const GetItemEntry& entry) {
    if (!StoryCoopActive() || m_skipItemGive)
        return;

    m_storyDirty = true;

    if (!m_partySettings.shareAmmoRupeesHp || !StoryIsSharedConsumable(entry))
        return;

    // the first stick, nut, bombchu or slingshot is an unlock and travels with the progress snapshot instead
    if (entry.modIndex == MOD_NONE) {
        int slot = -1;
        switch (entry.itemId) {
            case ITEM_STICK:
            case ITEM_STICKS_5:
            case ITEM_STICKS_10:
                slot = SLOT_STICK;
                break;
            case ITEM_NUT:
            case ITEM_NUTS_5:
            case ITEM_NUTS_10:
                slot = SLOT_NUT;
                break;
            case ITEM_BOMBCHU:
            case ITEM_BOMBCHUS_5:
            case ITEM_BOMBCHUS_20:
                slot = SLOT_BOMBCHU;
                break;
            case ITEM_SEEDS:
            case ITEM_SEEDS_30:
                slot = SLOT_SLINGSHOT;
                break;
            default:
                break;
        }
        if (slot >= 0 && m_storyLastSent.items[slot] == ITEM_NONE)
            return;
    }

    WritePacket(newPacket(CLIENT_PACKET_STORY_SHARE_ITEM)
                << PackedUInt2((u16)entry.modIndex) << PackedUInt2((u16)entry.getItemId));
}

void ZeldaOnlineClient::OnStoryShareItem(ByteStream& packet) {
    if (packet.BytesLeft() < 6)
        return;

    u16 senderID = (u16)packet.Read<PackedUInt2>().value();
    u16 modIndex = (u16)packet.Read<PackedUInt2>().value();
    u16 getItemId = (u16)packet.Read<PackedUInt2>().value();
    (void)senderID;

    if (!StoryShareConsumables() || gPlayState == nullptr || gSaveContext.gameMode != GAMEMODE_NORMAL)
        return;

    GetItemEntry entry;
    if (modIndex == MOD_NONE) {
        entry = ItemTableManager::Instance->RetrieveItemEntry(MOD_NONE, getItemId);
    } else if (modIndex == MOD_RANDOMIZER) {
        entry = Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(getItemId)).GetGIEntry_Copy();
    } else {
        return;
    }

    if (entry.getItemId == GI_NONE || !StoryIsSharedConsumable(entry))
        return;

    bool previousSkip = m_skipItemGive;
    m_skipItemGive = true;
    if (entry.modIndex == MOD_NONE)
        Item_Give(gPlayState, static_cast<u8>(entry.itemId));
    else
        Randomizer_Item_Give(gPlayState, entry);
    m_skipItemGive = previousSkip;
}

// ---------------------------------------------------------------------------------------------------------------
// Time travel together: the Master Sword pedestal vote
// ---------------------------------------------------------------------------------------------------------------

void ZeldaOnlineClient::StorySendVote(bool vote) {
    WritePacket(newPacket(CLIENT_PACKET_STORY_TIME_VOTE) << PackedUInt1(vote ? 1u : 0u) << PackedUInt1(LINK_IS_ADULT ? 0u : 1u));
}

void ZeldaOnlineClient::StoryReleaseHold(bool restorePlayer) {
    if (!m_storyHold)
        return;

    m_storyHold = false;
    m_storyMissing.clear();

    if (restorePlayer)
        StoryRestorePlayer();
}

// VB_MASTER_SWORD_PEDESTAL_USE: *should is "Link has taken the sword" (child pull or adult place back).
void ZeldaOnlineClient::StoryOnPedestalUse(Actor* pedestal, bool* should) {
    if (!StoryCoopActive())
        return;

    // everybody voted: let the native sequence run (first pull: Chamber of Sages, later: Temple of Time swap)
    if (m_storyApprovedFrames > 0) {
        m_storyApprovedFrames = 0;
        *should = true;
        return;
    }

    if (!*should)
        return;

    // Link took the sword: put it back (vanilla releases the parent in this branch too) and wait for the others
    if (pedestal != nullptr)
        pedestal->parent = nullptr;

    if (!m_storyHold) {
        m_storyHold = true;
        m_storyVoted = 1;
        m_storyTotal = m_partySize;
        m_storyMissing.clear();
        StorySendVote(true);
    }

    *should = false;
}

// VB_SWITCH_AGE: the SwitchAge cheat and the Ocarina Time Travel enhancement. There is no remote Song of Time path in
// ZeldaOnline (remote ocarinas are audio only), so only the local player can get here.
void ZeldaOnlineClient::StoryOnSwitchAge(bool* should) {
    if (!StoryCoopActive() || m_storyAllowAgeChange)
        return;

    *should = false;

    if (m_storyNotifyCooldown == 0) {
        Notification::Emit({ .message = "Story Co-op: use the Master Sword pedestal to time travel" });
        m_storyNotifyCooldown = 5 * 20;
    }
}

void ZeldaOnlineClient::StoryOnSceneInit() {
    // a scene change ends any wait: withdraw the vote so the others do not wait for somebody who left
    if (m_storyHold) {
        StorySendVote(false);
        m_storyHold = false;
        m_storyMissing.clear();
    }
    m_storyApprovedFrames = 0;
}

std::string ZeldaOnlineClient::StoryHoldText() const {
    std::string text = "Waiting for the others (" + std::to_string(m_storyVoted) + "/" + std::to_string(m_storyTotal) + ")";

    if (!m_storyMissing.empty()) {
        text += ": ";
        for (size_t i = 0; i < m_storyMissing.size(); i++) {
            if (i != 0)
                text += ", ";
            text += m_storyMissing[i];
        }
    }
    text += ". Press B to cancel.";

    // wrap on spaces to fit the screen
    const size_t maxLine = 44;
    std::string wrapped;
    size_t lineLen = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(' ', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string word = text.substr(pos, end - pos);
        if (lineLen != 0 && lineLen + 1 + word.size() > maxLine) {
            wrapped += '\n';
            lineLen = 0;
        } else if (lineLen != 0) {
            wrapped += ' ';
            lineLen++;
        }
        wrapped += word;
        lineLen += word.size();
        pos = end + 1;
    }
    return wrapped;
}

void ZeldaOnlineClient::OnStoryVoteStatus(ByteStream& packet) {
    if (packet.BytesLeft() < 5)
        return;

    u8 active = (u8)packet.Read<PackedUInt1>().value();
    u8 voted = (u8)packet.Read<PackedUInt1>().value();
    u8 total = (u8)packet.Read<PackedUInt1>().value();
    u8 travelling = (u8)packet.Read<PackedUInt1>().value();
    u8 missingCount = (u8)packet.Read<PackedUInt1>().value();

    std::vector<std::string> missing;
    for (u8 i = 0; i < missingCount; i++) {
        if (packet.BytesLeft() < 1)
            return;
        unsigned int len = packet.Read<PackedUInt1>().value();
        if (packet.BytesLeft() < len)
            return;
        missing.push_back(packet.ReadString(len));
    }

    if (!StoryCoopActive())
        return;

    if (active) {
        m_storyVoted = voted;
        m_storyTotal = total;
        m_storyMissing = missing;
    } else if (m_storyHold) {
        // no vote is running for this player any more: the vote was rejected (wrong era, the SET_ERA that came
        // with it is applied once the hold is gone), the votes were reset, or the party is already travelling
        StoryReleaseHold(true);
    }
}

void ZeldaOnlineClient::OnStoryTravel(ByteStream& packet) {
    if (packet.BytesLeft() < 1)
        return;

    packet.Read<PackedUInt1>();

    if (!StoryCoopActive() || !m_storyHold)
        return;

    // every member voted: the pedestal runs the native sequence on the next actor update, on every client at once
    m_storyHold = false;
    m_storyMissing.clear();
    m_storyApprovedFrames = 5 * 20;
}

void ZeldaOnlineClient::OnStorySetEra(ByteStream& packet) {
    if (packet.BytesLeft() < 1)
        return;

    u8 era = (u8)packet.Read<PackedUInt1>().value();

    if (!StoryCoopActive() || era > 1)
        return;

    m_storyMoveEra = era;
}

// Moves this player to the era the server asked for (late joiner, a missed travel). Waits for a quiet moment.
void ZeldaOnlineClient::StoryMoveToEra() {
    if (m_storyMoveEra < 0 || m_storyHold || m_storyApprovedFrames > 0 || !StoryCanApply())
        return;

    bool wantAdult = m_storyMoveEra == 0;
    m_storyMoveEra = -1;

    if (LINK_IS_ADULT == wantAdult)
        return;

    if (wantAdult) {
        // an adult needs the Master Sword and the "pulled it" story state, or the Temple of Time would not make sense
        Flags_SetEventChkInf(EVENTCHKINF_PULLED_MASTER_SWORD_FROM_PEDESTAL);
        Flags_SetEventChkInf(EVENTCHKINF_ENTERED_MASTER_SWORD_CHAMBER);
        gSaveContext.inventory.equipment |= OWNED_EQUIP_FLAG(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_MASTER);
    }

    Notification::Emit({ .message = "Moving to the party's era" });

    m_storyAllowAgeChange = true;
    SwitchAge();
    m_storyAllowAgeChange = false;
}
