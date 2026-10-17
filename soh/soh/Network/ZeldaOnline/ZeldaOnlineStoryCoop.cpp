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
}

namespace {

using namespace ZeldaOnline;

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

// Slots that are never shared as slot items (bottles and trade items are per player, beans are bought)
bool StorySlotIsPerPlayer(int slot) {
    return StoryIsBottleSlot(slot) || slot == SLOT_TRADE_ADULT || slot == SLOT_TRADE_CHILD || slot == STORY_SLOT_BEAN;
}

// eventChkInf and infTable masks of the bits that must never be shared
void StoryEventChkInfMask(uint16_t out[14]) {
    for (int i = 0; i < 14; i++)
        out[i] = 0xFFFF;
    for (int flag : STORY_EVENTCHKINF_EXCLUDE) {
        if ((flag >> 4) < 14)
            out[flag >> 4] &= (uint16_t)~(1 << (flag & 0xF));
    }
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

void ZeldaOnlineClient::StoryMarkDirty() {
    if (StoryCoopActive())
        m_storyDirty = true;
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
    StoryEventChkInfMask(eventMask);
    for (int i = 0; i < 14; i++)
        p.eventChkInf[i] = gSaveContext.eventChkInf[i] & eventMask[i];
    for (int i = 0; i < 4; i++)
        p.itemGetInf[i] = gSaveContext.itemGetInf[i];
    for (int i = 0; i < 30; i++)
        p.infTable[i] = (i == STORY_INFTABLE_EXCLUDE_INDEX) ? 0 : gSaveContext.infTable[i];
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
            scene.swch = gPlayState->actorCtx.flags.swch;
            scene.clear = gPlayState->actorCtx.flags.clear;
            scene.collect = gPlayState->actorCtx.flags.collect;
        }

        // dungeons and boss rooms use per party server instances for switches and clear flags
        if (StoryIsPartyScene(s)) {
            scene.swch = 0;
            scene.clear = 0;
        }
        if (s == SCENE_WATER_TEMPLE)
            scene.swch &= ~STORY_WATER_TEMPLE_SWCH_EXCLUDE;
        if (s == SCENE_FOREST_TEMPLE)
            scene.swch &= ~STORY_FOREST_TEMPLE_SWCH_EXCLUDE;

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
    StoryEventChkInfMask(eventMask);
    for (int i = 0; i < 14; i++)
        gSaveContext.eventChkInf[i] |= (team.eventChkInf[i] & eventMask[i]);
    for (int i = 0; i < 4; i++)
        gSaveContext.itemGetInf[i] |= team.itemGetInf[i];
    for (int i = 0; i < 30; i++) {
        if (i != STORY_INFTABLE_EXCLUDE_INDEX)
            gSaveContext.infTable[i] |= team.infTable[i];
    }
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

    // slot items (bottles, trade items and beans are per player and never in the snapshot)
    for (int slot = 0; slot < 24; slot++) {
        if (StorySlotIsPerPlayer(slot))
            continue;

        u8 teamItem = team.items[slot];
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
        u32 swch = party ? 0 : scene.swch;
        u32 clear = party ? 0 : scene.clear;
        if (scene.sceneNum == SCENE_WATER_TEMPLE)
            swch &= ~STORY_WATER_TEMPLE_SWCH_EXCLUDE;
        if (scene.sceneNum == SCENE_FOREST_TEMPLE)
            swch &= ~STORY_FOREST_TEMPLE_SWCH_EXCLUDE;

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

    packet.Read<PackedUInt1>();

    StoryProgress incoming;
    if (!incoming.Deserialize(packet))
        return;

    if (!StoryCoopActive())
        return;

    if (!m_storyHasPending) {
        m_storyPending = StoryProgress();
        m_storyHasPending = true;
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
