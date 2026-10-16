#include "soh/Network/Anchor/BossSync/BossSyncDispatch.h"
#include "soh/Network/Anchor/BossSync/BossSyncRegistry.h"

namespace AnchorBossSync {

nlohmann::json CaptureBossTransition(PlayState* play, Actor* actor) {
    if (play == nullptr || actor == nullptr) {
        return {};
    }

    auto adapter = FindBossSyncAdapter(play->sceneNum, actor->id);
    if (!adapter) {
        return {};
    }

    return adapter->CaptureTransition(play, actor);
}

void TickBossFollower(PlayState* play, Actor* actor) {
    if (play == nullptr || actor == nullptr) {
        return;
    }
    auto adapter = FindBossSyncAdapter(play->sceneNum, actor->id);
    if (adapter) {
        adapter->FollowerTick(play, actor);
    }
}

bool ApplyBossEvent(PlayState* play, s16 sceneNum, s16 actorId, const nlohmann::json& payload) {
    if (play == nullptr) {
        return false;
    }

    auto adapter = FindBossSyncAdapter(sceneNum, actorId);
    if (!adapter) {
        return false;
    }

    // Boss event payloads are ROOM_EVENT wrappers: adapter-specific fields live
    // under payload["eventData"], but adapters access them via payload.value().
    // Merge eventData fields into the root so adapters can use flat key access
    // (e.g. payload.value("hp", 1)) without knowing the nesting structure.
    // Root-level keys (eventType, sceneNum, seq, ...) keep priority.
    nlohmann::json merged = payload;
    if (payload.contains("eventData") && payload["eventData"].is_object()) {
        for (auto& [k, v] : payload["eventData"].items()) {
            if (!merged.contains(k)) {
                merged[k] = v;
            }
        }
    }

    adapter->ApplyEvent(play, merged);
    return true;
}

void ApplyBossSnapshot(PlayState* play, s16 sceneNum, s16 actorId, const nlohmann::json& snapshot) {
    if (play == nullptr) {
        return;
    }

    auto adapter = FindBossSyncAdapter(sceneNum, actorId);
    if (!adapter) {
        return;
    }

    // Same flat-merge as ApplyBossEvent so snapshot fields are reachable
    // via payload.value("hp", ...) regardless of nesting depth.
    nlohmann::json merged = snapshot;
    if (snapshot.contains("eventData") && snapshot["eventData"].is_object()) {
        for (auto& [k, v] : snapshot["eventData"].items()) {
            if (!merged.contains(k)) {
                merged[k] = v;
            }
        }
    }

    adapter->ApplySnapshot(play, merged);
}

} // namespace AnchorBossSync
