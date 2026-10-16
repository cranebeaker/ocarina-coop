#pragma once

#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
}

namespace AnchorBossSync {

nlohmann::json CaptureBossTransition(PlayState* play, Actor* actor);

// Returns true if a registered adapter handled the event.
bool ApplyBossEvent(PlayState* play, s16 sceneNum, s16 actorId, const nlohmann::json& payload);

// Called from RoomSnapshot apply path so late-joiners get authoritative boss HP.
void ApplyBossSnapshot(PlayState* play, s16 sceneNum, s16 actorId, const nlohmann::json& snapshot);

// Per-frame follower reconcile; no-op for adapters that do not override FollowerTick.
void TickBossFollower(PlayState* play, Actor* actor);

} // namespace AnchorBossSync
