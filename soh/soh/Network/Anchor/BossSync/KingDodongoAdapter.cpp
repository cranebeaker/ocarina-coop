#include "soh/Network/Anchor/BossSync/BossSyncRegistry.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Boss_Dodongo/z_boss_dodongo.h"

// All non-static in z_boss_dodongo.c (same situation as BossGoma_SetupDefeated).
void BossDodongo_SetupWalk(BossDodongo* b);
void BossDodongo_SetupRoll(BossDodongo* b);
void BossDodongo_SetupInhale(BossDodongo* b);
void BossDodongo_SetupBlowFire(BossDodongo* b);
void BossDodongo_SetupExplode(BossDodongo* b);
void BossDodongo_SetupDamaged(BossDodongo* b);
void BossDodongo_SetupDeathCutscene(BossDodongo* b);
void BossDodongo_SpawnFire(BossDodongo* b, PlayState* play, s16 params);
void BossDodongo_IntroCutscene(BossDodongo* b, PlayState* play);
void BossDodongo_Walk(BossDodongo* b, PlayState* play);
void BossDodongo_Roll(BossDodongo* b, PlayState* play);
void BossDodongo_Inhale(BossDodongo* b, PlayState* play);
void BossDodongo_BlowFire(BossDodongo* b, PlayState* play);
void BossDodongo_Explode(BossDodongo* b, PlayState* play);
void BossDodongo_LayDown(BossDodongo* b, PlayState* play);
void BossDodongo_Vulnerable(BossDodongo* b, PlayState* play);
void BossDodongo_GetUp(BossDodongo* b, PlayState* play);
void BossDodongo_Damaged(BossDodongo* b, PlayState* play);
void BossDodongo_DeathCutscene(BossDodongo* b, PlayState* play);
}

// King Dodongo (BossDodongo) does NOT use actor.colChkInfo.health for combat.
// It keeps a dedicated `health` field (s16; accessed via the struct, since the
// 64-bit PC build does not share the N64 offsets) and detects
// death via `this->health <= 0` — the shared GenericBossHealthPhaseAdapter
// reads colChkInfo.health, which King Dodongo never changes, so HP/phase/
// death sync was completely inert for this boss. See z_boss_dodongo.h/.c.
namespace AnchorBossSync {

namespace {

enum KdAction {
    KD_UNKNOWN = -1,
    KD_INTRO = 0,
    KD_WALK,
    KD_ROLL,
    KD_INHALE,
    KD_BLOWFIRE,
    KD_EXPLODE,
    KD_LAYDOWN,
    KD_VULNERABLE,
    KD_GETUP,
    KD_DAMAGED,
    KD_DEATH,
};

int KdActionId(const BossDodongo* b) {
    const auto f = b->actionFunc;
    if (f == BossDodongo_IntroCutscene) return KD_INTRO;
    if (f == BossDodongo_Walk) return KD_WALK;
    if (f == BossDodongo_Roll) return KD_ROLL;
    if (f == BossDodongo_Inhale) return KD_INHALE;
    if (f == BossDodongo_BlowFire) return KD_BLOWFIRE;
    if (f == BossDodongo_Explode) return KD_EXPLODE;
    if (f == BossDodongo_LayDown) return KD_LAYDOWN;
    if (f == BossDodongo_Vulnerable) return KD_VULNERABLE;
    if (f == BossDodongo_GetUp) return KD_GETUP;
    if (f == BossDodongo_Damaged) return KD_DAMAGED;
    if (f == BossDodongo_DeathCutscene) return KD_DEATH;
    return KD_UNKNOWN;
}

// Last host state received by a follower (for reconcile).
struct HostKdState {
    int action = KD_UNKNOWN;
    int corner = -1;
    int dir = -1;
    uint32_t recvFrame = 0;
    uint32_t mismatchSince = 0; // 0 = currently matching
};
std::unordered_map<std::string, HostKdState> gHostKd;

// Forces the follower KD into the host action using the original Setup functions.
// LayDown/Vulnerable/GetUp have no Setup; they follow deterministically from Explode/Damaged.
void ApplyKdAction(BossDodongo* b, PlayState* play, int action, int corner, int dir) {
    const int local = KdActionId(b);
    if (local == KD_INTRO || local == KD_DEATH || local == KD_UNKNOWN) return; // never fight cutscenes
    if (action == KD_INTRO || action == KD_UNKNOWN) return;

    if (corner >= 0 && corner <= 3) b->unk_1A0 = (s16)corner;
    if (dir == 0 || dir == 1) b->unk_1A2 = (s16)dir;

    if (local == action) return;

    switch (action) {
        case KD_WALK:      BossDodongo_SetupWalk(b); break;
        case KD_ROLL:      BossDodongo_SetupRoll(b); break;
        case KD_INHALE:
            BossDodongo_SetupInhale(b);
            BossDodongo_SpawnFire(b, play, -1);
            break;
        case KD_BLOWFIRE:  BossDodongo_SetupBlowFire(b); break;
        case KD_EXPLODE:   BossDodongo_SetupExplode(b); break;
        case KD_DAMAGED:   BossDodongo_SetupDamaged(b); break;
        case KD_DEATH:     BossDodongo_SetupDeathCutscene(b); break;
        default: break; // LayDown / Vulnerable / GetUp: local chain arrives on its own
    }
}

struct DodongoTrackState {
    bool initialized = false;
    int16_t initialHp = 1;
    int16_t lastHp = 1;
    int lastPhaseId = -1;
    int lastAction = -1;
    int lastCorner = -1;
    int lastDir = -1;
    std::deque<nlohmann::json> pendingEvents;
};

std::unordered_map<std::string, DodongoTrackState> gDodongoTrack;

std::string BuildActorKeyLocal(const Actor* actor, s16 sceneNum) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%d_%d_%d_%d_%d_%d_%d_%d_%d_%d_%d",
             (int)sceneNum,
             (int)actor->category,
             (int)actor->id,
             (int)actor->room,
             (int)actor->params,
             (int)actor->home.pos.x,
             (int)actor->home.pos.y,
             (int)actor->home.pos.z,
             (int)actor->home.rot.x,
             (int)actor->home.rot.y,
             (int)actor->home.rot.z);
    return std::string(buf);
}

// Reads/writes the boss-local `health` field directly, clamped to >= 0 since
// the game itself allows it to go negative before checking `<= 0` for death.
int16_t ReadDodongoHealth(const Actor* actor) {
    const int16_t raw = ((const BossDodongo*)actor)->health;
    return (raw < 0) ? 0 : raw;
}

void WriteDodongoHealth(Actor* actor, int16_t value) {
    ((BossDodongo*)actor)->health = (value < 0) ? 0 : value;
}

int ComputePhaseByHp(int16_t hp, int16_t initialHp) {
    if (initialHp <= 1) {
        return hp <= 0 ? 3 : 0;
    }

    float ratio = (float)hp / (float)std::max<int16_t>(initialHp, 1);
    if (hp <= 0) return 3;
    if (ratio <= 0.33f) return 2;
    if (ratio <= 0.66f) return 1;
    return 0;
}

class KingDodongoAdapter : public BossSyncAdapter {
  public:
    bool CanHandle(s16 sceneNum, s16 actorId) const override {
        return sceneNum == SCENE_DODONGOS_CAVERN_BOSS && actorId == ACTOR_BOSS_DODONGO;
    }

    const char* Name() const override {
        return "KingDodongoAdapter";
    }

    void Reset() override {
        gDodongoTrack.clear();
        gHostKd.clear();
    }

    // RoomEvent accepts one boss event per frame (seq dedup), so action and HP events are
    // queued and drained one per call.
    nlohmann::json CaptureTransition(PlayState* play, Actor* actor) override {
        if (play == nullptr || actor == nullptr) {
            return {};
        }

        const std::string bossActorKey = BuildActorKeyLocal(actor, play->sceneNum);
        DodongoTrackState& track = gDodongoTrack[bossActorKey];

        const BossDodongo* kd = (const BossDodongo*)actor;
        const int action = KdActionId(kd);
        const int corner = kd->unk_1A0;
        const int dir = kd->unk_1A2;
        if (action != track.lastAction || corner != track.lastCorner || dir != track.lastDir) {
            track.lastAction = action;
            track.lastCorner = corner;
            track.lastDir = dir;

            nlohmann::json ev;
            ev["eventType"] = "BOSS_ACTION_STATE";
            ev["eventKey"] = bossActorKey;
            ev["bossActorKey"] = bossActorKey;
            ev["bossActorId"] = (int)actor->id;
            ev["action"] = action;
            ev["corner"] = corner;
            ev["dir"] = dir;
            ev["hp"] = (int)ReadDodongoHealth(actor);
            ev["seq"] = (uint32_t)play->state.frames;
            ev["masterFrame"] = (uint32_t)play->state.frames;
            ev["lateJoinCanSkipIntro"] = true;
            track.pendingEvents.push_back(ev);
        }

        nlohmann::json hpEvent = CaptureHpTransition(play, actor, bossActorKey, track);
        if (!hpEvent.empty()) {
            track.pendingEvents.push_back(hpEvent);
        }

        if (!track.pendingEvents.empty()) {
            nlohmann::json next = track.pendingEvents.front();
            track.pendingEvents.pop_front();
            return next;
        }
        return {};
    }

    nlohmann::json CaptureHpTransition(PlayState* play, Actor* actor, const std::string& bossActorKey,
                                       DodongoTrackState& track) {

        const int16_t hp = ReadDodongoHealth(actor);
        if (!track.initialized) {
            track.initialized = true;
            track.initialHp = std::max<int16_t>(hp, 1);
            track.lastHp = hp;
            track.lastPhaseId = ComputePhaseByHp(hp, track.initialHp);

            nlohmann::json event;
            event["eventType"] = "BOSS_STAGE_ENTER";
            event["eventKey"] = bossActorKey;
            event["bossActorKey"] = bossActorKey;
            event["bossActorId"] = (int)actor->id;
            event["phaseId"] = track.lastPhaseId;
            event["hp"] = (int)hp;
            event["seq"] = (uint32_t)play->state.frames;
            event["masterFrame"] = (uint32_t)play->state.frames;
            event["lateJoinCanSkipIntro"] = true;
            return event;
        }

        const int phaseNow = ComputePhaseByHp(hp, track.initialHp);

        if (hp <= 0 && track.lastHp > 0) {
            track.lastHp = hp;
            track.lastPhaseId = phaseNow;

            nlohmann::json event;
            event["eventType"] = "BOSS_DEATH_COMMIT";
            event["eventKey"] = bossActorKey;
            event["bossActorKey"] = bossActorKey;
            event["bossActorId"] = (int)actor->id;
            event["phaseId"] = phaseNow;
            event["hp"] = 0;
            event["seq"] = (uint32_t)play->state.frames;
            event["masterFrame"] = (uint32_t)play->state.frames;
            event["lateJoinCanSkipIntro"] = true;
            return event;
        }

        if (phaseNow != track.lastPhaseId) {
            track.lastPhaseId = phaseNow;
            track.lastHp = hp;

            nlohmann::json event;
            event["eventType"] = "BOSS_STAGE_ENTER";
            event["eventKey"] = bossActorKey;
            event["bossActorKey"] = bossActorKey;
            event["bossActorId"] = (int)actor->id;
            event["phaseId"] = phaseNow;
            event["hp"] = (int)hp;
            event["seq"] = (uint32_t)play->state.frames;
            event["masterFrame"] = (uint32_t)play->state.frames;
            event["lateJoinCanSkipIntro"] = true;
            return event;
        }

        if (hp < track.lastHp) {
            const int16_t oldHp = track.lastHp;
            track.lastHp = hp;

            nlohmann::json event;
            event["eventType"] = "BOSS_WEAKPOINT_HIT";
            event["eventKey"] = bossActorKey;
            event["bossActorKey"] = bossActorKey;
            event["bossActorId"] = (int)actor->id;
            event["phaseId"] = phaseNow;
            event["hp"] = (int)hp;
            event["damage"] = (int)(oldHp - hp);
            event["seq"] = (uint32_t)play->state.frames;
            event["masterFrame"] = (uint32_t)play->state.frames;
            event["lateJoinCanSkipIntro"] = true;
            return event;
        }

        if (hp != track.lastHp) {
            track.lastHp = hp;
        }

        return {};
    }

    void ApplyEvent(PlayState* play, const nlohmann::json& payload) override {
        if (!IsPlaySessionActive(play)) return;

        const std::string eventType    = payload.value("eventType", std::string(""));
        const std::string bossActorKey = payload.value("bossActorKey", std::string(""));
        const int16_t     hp           = (int16_t)payload.value("hp", 1);
        const s16         bossActorId  = (s16)payload.value("bossActorId", (int)-1);
        if (bossActorKey.empty() || bossActorId < 0) return;

        Actor* boss = nullptr;
        for (int cat : { ACTORCAT_BOSS, ACTORCAT_ENEMY }) {
            Actor* actor = play->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                if (actor->id == bossActorId &&
                    BuildActorKeyLocal(actor, play->sceneNum) == bossActorKey) {
                    boss = actor;
                    break;
                }
                actor = actor->next;
            }
            if (boss) break;
        }
        if (!boss || !boss->update) return;

        if (eventType == "BOSS_ACTION_STATE") {
            const int action = payload.value("action", (int)KD_UNKNOWN);
            const int corner = payload.value("corner", -1);
            const int dir = payload.value("dir", -1);
            HostKdState& st = gHostKd[bossActorKey];
            st.action = action;
            st.corner = corner;
            st.dir = dir;
            st.recvFrame = (uint32_t)play->state.frames;
            st.mismatchSince = 0;
            ApplyKdAction((BossDodongo*)boss, play, action, corner, dir);
            return;
        }

        if (eventType == "BOSS_DEATH_COMMIT") {
            // Native UpdateDamage sees health <= 0 and starts the death cutscene.
            WriteDodongoHealth(boss, 0);
            return;
        }

        if (eventType == "BOSS_WEAKPOINT_HIT" || eventType == "BOSS_STAGE_ENTER") {
            // Only apply downward HP changes — stale events must never heal the boss.
            if (hp < ReadDodongoHealth(boss) || hp <= 0) {
                WriteDodongoHealth(boss, hp);
            }
            if (eventType == "BOSS_WEAKPOINT_HIT") {
                Actor_SetColorFilter(boss, 0x4000, 0xFF, 0, 8);
            }
        }
    }

    nlohmann::json BuildSnapshot(PlayState* play, Actor* actor) override {
        if (play == nullptr || actor == nullptr) {
            return {};
        }

        const std::string bossActorKey = BuildActorKeyLocal(actor, play->sceneNum);
        const int16_t hp = ReadDodongoHealth(actor);

        nlohmann::json snap;
        snap["bossActorKey"] = bossActorKey;
        snap["bossActorId"] = (int)actor->id;
        snap["sceneNum"] = play->sceneNum;
        snap["roomNum"] = (int)play->roomCtx.curRoom.num;
        snap["phaseId"] = ComputePhaseByHp(hp, gDodongoTrack.count(bossActorKey) ? gDodongoTrack[bossActorKey].initialHp : std::max<int16_t>(hp, 1));
        snap["hp"] = (int)hp;
        snap["action"] = KdActionId((const BossDodongo*)actor);
        snap["corner"] = (int)((const BossDodongo*)actor)->unk_1A0;
        snap["dir"] = (int)((const BossDodongo*)actor)->unk_1A2;
        snap["lateJoinCanSkipIntro"] = true;
        return snap;
    }

    void FollowerTick(PlayState* play, Actor* actor) override {
        if (play == nullptr || actor == nullptr || actor->update == nullptr) return;
        auto it = gHostKd.find(BuildActorKeyLocal(actor, play->sceneNum));
        if (it == gHostKd.end()) return;
        HostKdState& st = it->second;
        BossDodongo* b = (BossDodongo*)actor;
        const int local = KdActionId(b);
        const uint32_t now = (uint32_t)play->state.frames;
        // Matching, or states that follow deterministically / must not be forced.
        if (local == st.action || local == KD_INTRO || local == KD_DEATH || local == KD_UNKNOWN ||
            st.action == KD_LAYDOWN || st.action == KD_VULNERABLE || st.action == KD_GETUP) {
            st.mismatchSince = 0;
            return;
        }
        if (st.mismatchSince == 0) {
            st.mismatchSince = now;
            return;
        }
        if (now - st.mismatchSince > 10) {
            st.mismatchSince = 0;
            ApplyKdAction(b, play, st.action, st.corner, st.dir);
        }
    }

    void ApplySnapshot(PlayState* play, const nlohmann::json& snapshot) override {
        if (!IsPlaySessionActive(play)) return;

        const std::string bossActorKey = snapshot.value("bossActorKey", std::string(""));
        const s16         bossActorId  = (s16)snapshot.value("bossActorId", (int)-1);
        const int16_t     hp           = (int16_t)snapshot.value("hp", 1);
        if (bossActorKey.empty() || bossActorId < 0) return;

        for (int cat : { ACTORCAT_BOSS, ACTORCAT_ENEMY }) {
            Actor* actor = play->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                if (actor->id == bossActorId &&
                    BuildActorKeyLocal(actor, play->sceneNum) == bossActorKey &&
                    actor->update != nullptr) {
                    // Snapshot sets absolute HP — this is the authoritative initial state
                    // for the late joiner, so we apply regardless of direction.
                    WriteDodongoHealth(actor, hp);
                    const int action = snapshot.value("action", (int)KD_UNKNOWN);
                    const int corner = snapshot.value("corner", -1);
                    const int dir = snapshot.value("dir", -1);
                    HostKdState& st = gHostKd[bossActorKey];
                    st.action = action;
                    st.corner = corner;
                    st.dir = dir;
                    st.recvFrame = (uint32_t)play->state.frames;
                    st.mismatchSince = 0;
                    ApplyKdAction((BossDodongo*)actor, play, action, corner, dir);
                    return;
                }
                actor = actor->next;
            }
        }
    }
};

} // namespace

std::shared_ptr<BossSyncAdapter> CreateKingDodongoAdapter() {
    return std::make_shared<KingDodongoAdapter>();
}

} // namespace AnchorBossSync
