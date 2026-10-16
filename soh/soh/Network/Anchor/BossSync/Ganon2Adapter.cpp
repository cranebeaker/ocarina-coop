#include "soh/Network/Anchor/BossSync/BossSyncRegistry.h"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "variables.h"
#include "functions.h"
}

namespace AnchorBossSync {

namespace {

struct Ganon2TrackState {
    bool initialized = false;
    int lastHp = 0;
    int lastPhaseId = -1;
    std::deque<nlohmann::json> pendingEvents;
};

std::unordered_map<std::string, Ganon2TrackState> gGanon2Track;

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

// Real health lives in colChkInfo.health (starts at 30, knockdown below 21, death at 0).
// The float at 0x324 is the arena fire-ring alpha and must not be read or written.
int ComputePhaseFromGanon2Health(int hp) {
    if (hp <= 0) {
        return 3;
    }
    if (hp < 21) {
        return 1;
    }
    return 0;
}

class Ganon2Adapter : public BossSyncAdapter {
  public:
    bool CanHandle(s16 sceneNum, s16 actorId) const override {
        return sceneNum == SCENE_GANON_BOSS && actorId == ACTOR_BOSS_GANON2;
    }

    const char* Name() const override {
        return "Ganon2Adapter";
    }

    void Reset() override {
        gGanon2Track.clear();
    }

    nlohmann::json CaptureTransition(PlayState* play, Actor* actor) override {
        if (play == nullptr || actor == nullptr) {
            return {};
        }

        if (actor->id != ACTOR_BOSS_GANON2) {
            return {};
        }

        const std::string ganon2Key = BuildActorKeyLocal(actor, play->sceneNum);
        Ganon2TrackState& track = gGanon2Track[ganon2Key];

        const int hp = (int)ReadClampedBossHealth(actor);
        const int phaseNow = ComputePhaseFromGanon2Health(hp);

        auto makeEvent = [&](const char* type, int phase) {
            nlohmann::json event;
            event["eventType"] = type;
            event["eventKey"] = ganon2Key;
            event["bossActorKey"] = ganon2Key;
            event["bossActorId"] = (int)actor->id;
            event["phaseId"] = phase;
            event["hp"] = hp;
            event["seq"] = (uint32_t)play->state.frames;
            event["masterFrame"] = (uint32_t)play->state.frames;
            event["lateJoinCanSkipIntro"] = true;
            return event;
        };

        if (!track.initialized) {
            track.initialized = true;
            track.lastHp = hp;
            track.lastPhaseId = phaseNow;
            nlohmann::json event = makeEvent("BOSS_STAGE_ENTER", phaseNow);
            event["subState"] = "ganon2_intro";
            return event;
        }

        // No BOSS_DEATH_COMMIT here: Ganon's death (and phase-1 knockdown) only start from his own
        // collision callback; a remote hp=0 would leave the peer's boss stuck without a sequence.
        if (hp < track.lastHp) {
            const int oldHp = track.lastHp;
            track.lastHp = hp;
            track.lastPhaseId = phaseNow;
            nlohmann::json event = makeEvent("BOSS_WEAKPOINT_HIT", phaseNow);
            event["damage"] = oldHp - hp;
            return event;
        }

        if (phaseNow != track.lastPhaseId) {
            track.lastPhaseId = phaseNow;
            track.lastHp = hp;
            nlohmann::json event = makeEvent("BOSS_STAGE_ENTER", phaseNow);
            event["subState"] = "phase_transition";
            return event;
        }

        if (hp != track.lastHp) {
            track.lastHp = hp;
        }

        if (!track.pendingEvents.empty()) {
            nlohmann::json next = track.pendingEvents.front();
            track.pendingEvents.pop_front();
            return next;
        }

        return {};
    }

    void ApplyEvent(PlayState* play, const nlohmann::json& payload) override {
        if (!IsPlaySessionActive(play)) return;

        const std::string eventType = payload.value("eventType", std::string(""));
        const std::string bossActorKey = payload.value("bossActorKey", std::string(""));
        const int hp = payload.value("hp", -1);

        if (eventType == "BOSS_WEAKPOINT_HIT" || eventType == "BOSS_STAGE_ENTER") {
            Actor* ganon2 = nullptr;
            for (int cat : { ACTORCAT_BOSS, ACTORCAT_ENEMY }) {
                Actor* actor = play->actorCtx.actorLists[cat].head;
                while (actor != nullptr) {
                    if (BuildActorKeyLocal(actor, play->sceneNum) == bossActorKey && actor->id == ACTOR_BOSS_GANON2) {
                        ganon2 = actor;
                        break;
                    }
                    actor = actor->next;
                }
                if (ganon2) break;
            }
            if (ganon2 && ganon2->update != nullptr) {
                // Only lower health, never raise (stale events must not heal), and never to 0:
                // death starts from Ganon's own collision callback.
                const int current = (int)ReadClampedBossHealth(ganon2);
                if (hp >= 1 && hp < current) {
                    ganon2->colChkInfo.health = (u8)hp;
                }
                if (eventType == "BOSS_WEAKPOINT_HIT") {
                    Actor_SetColorFilter(ganon2, 0x4000, 0xFF, 0, 8);
                }
            }
        }
    }

    nlohmann::json BuildSnapshot(PlayState* play, Actor* actor) override {
        if (play == nullptr || actor == nullptr || actor->id != ACTOR_BOSS_GANON2) {
            return {};
        }

        const std::string ganon2Key = BuildActorKeyLocal(actor, play->sceneNum);
        const int hp = (int)ReadClampedBossHealth(actor);
        const int phaseNow = ComputePhaseFromGanon2Health(hp);

        nlohmann::json snap;
        snap["bossActorKey"] = ganon2Key;
        snap["bossActorId"] = (int)actor->id;
        snap["sceneNum"] = play->sceneNum;
        snap["roomNum"] = (int)play->roomCtx.curRoom.num;
        snap["phaseId"] = phaseNow;
        snap["hp"] = hp;
        snap["lateJoinCanSkipIntro"] = true;
        return snap;
    }

    void ApplySnapshot(PlayState* play, const nlohmann::json& snapshot) override {
        if (!IsPlaySessionActive(play)) return;

        const std::string bossActorKey = snapshot.value("bossActorKey", std::string(""));
        const int hp = snapshot.value("hp", -1);

        Actor* ganon2 = nullptr;
        for (int cat : { ACTORCAT_BOSS, ACTORCAT_ENEMY }) {
            Actor* actor = play->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                if (BuildActorKeyLocal(actor, play->sceneNum) == bossActorKey && actor->id == ACTOR_BOSS_GANON2) {
                    ganon2 = actor;
                    break;
                }
                actor = actor->next;
            }
            if (ganon2) break;
        }
        if (!ganon2 || !ganon2->update) return;

        const int current = (int)ReadClampedBossHealth(ganon2);
        if (hp >= 1 && hp < current) {
            ganon2->colChkInfo.health = (u8)hp;
        }
    }
};

} // namespace

std::shared_ptr<BossSyncAdapter> CreateGanon2Adapter() {
    return std::make_shared<Ganon2Adapter>();
}

} // namespace AnchorBossSync
