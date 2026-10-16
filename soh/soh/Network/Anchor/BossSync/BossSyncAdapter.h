#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"

extern SaveContext gSaveContext;
}

namespace AnchorBossSync {

inline bool IsPlaySessionActive(PlayState* play) {
  if (play == nullptr) {
    return false;
  }

  if (((Player*)play->actorCtx.actorLists[ACTORCAT_PLAYER].head) == nullptr) {
    return false;
  }

  if (gSaveContext.fileNum < 0 || gSaveContext.fileNum > 2) {
    return false;
  }

  if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
    return false;
  }

  return true;
}

// Several OoT bosses subtract damage from colChkInfo.health directly (bypassing
// the engine-safe Actor_ApplyDamage clamp helper) and detect death via a SIGNED
// (s8) <= 0 comparison without ever re-clamping the field back to 0. A lethal
// hit can overshoot past exactly zero and wrap to a large unsigned value
// (e.g. 251), which a naive `uint8_t hp = actor->colChkInfo.health;` read would
// interpret as "nearly full health" instead of "dead". Confirmed affected:
// Gohma, Ganondorf, Phantom Ganon, Morpha. Reading through this helper makes
// every adapter immune regardless of which specific boss it targets.
inline uint8_t ReadClampedBossHealth(const Actor* actor) {
  if (actor == nullptr) {
    return 0;
  }
  const int8_t signedHealth = (int8_t)actor->colChkInfo.health;
  return (signedHealth <= 0) ? 0 : (uint8_t)signedHealth;
}

// Adapter interface for a single boss/miniboss state machine.
// Implementations convert local actor transitions to canonical BOSS_* events
// and apply remote events/snapshots back into local actor state.
class BossSyncAdapter {
  public:
    virtual ~BossSyncAdapter() = default;

    virtual bool CanHandle(s16 sceneNum, s16 actorId) const = 0;

    virtual const char* Name() const = 0;

    // Called when the scene changes so per-adapter tracking maps are cleared.
    // Without this, a stale 'initialized=true' entry prevents BOSS_STAGE_ENTER
    // from being re-emitted after a player dies and the boss room is reloaded.
    virtual void Reset() {}

    virtual nlohmann::json CaptureTransition(PlayState* play, Actor* actor) = 0;

    virtual void ApplyEvent(PlayState* play, const nlohmann::json& payload) = 0;

    virtual nlohmann::json BuildSnapshot(PlayState* play, Actor* actor) = 0;

    virtual void ApplySnapshot(PlayState* play, const nlohmann::json& snapshot) = 0;

    // Per-frame follower hook (optional): lets an adapter re-converge on the last host state.
    virtual void FollowerTick(PlayState* play, Actor* actor) {}
};

} // namespace AnchorBossSync
