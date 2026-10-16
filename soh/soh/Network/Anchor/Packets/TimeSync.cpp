#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
extern u16 gTimeIncrement;
}

/**
 * TIME_SYNC
 *
 * Sent by the HOST to all clients every ~3 seconds (60 frames) when
 * "Sync Day/Night Cycle" is enabled.
 *
 * Payload:
 *   dayTime  – current gSaveContext.dayTime on the host
 *   frozen   – true if either player is in a timeless scene
 *              (gTimeIncrement == 0, i.e. dungeon / indoor / village)
 *
 * Rules:
 *   1. If EITHER player is in a timeless scene, dayTime is frozen for BOTH.
 *      The host undoes the Environment_Update() increment each frame until
 *      both are in time-advancing scenes again.
 *   2. The client simply overwrites its local dayTime with the received value
 *      and also suppresses its own local advancement when frozen == true.
 *
 * "Timeless scene" detection: gTimeIncrement is set to 0 by the game engine
 * for every indoor, dungeon, and fixed-time overworld area.  No hardcoded
 * scene list is needed – the engine already does the bookkeeping.
 */

void Anchor::SendPacket_TimeSync() {
    if (!IsSaveLoaded() || !gPlayState) return;

    bool localTimeless = (gTimeIncrement == 0);
    bool remoteTimeless = false;
    for (auto& [clientId, client] : clients) {
        if (!client.self && client.online && client.isSaveLoaded && client.timeIncrement == 0) {
            remoteTimeless = true;
            break;
        }
    }

    nlohmann::json payload;
    payload["type"]    = TIME_SYNC;
    payload["dayTime"] = (uint16_t)gSaveContext.dayTime;
    payload["frozen"]  = localTimeless || remoteTimeless;
    payload["night"]   = (uint8_t)gSaveContext.nightFlag;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_TimeSync(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;

    gSaveContext.dayTime = payload.value("dayTime", (uint16_t)gSaveContext.dayTime);
    remoteTimeFrozen    = payload.value("frozen", false);
    if (payload.contains("night")) {
        // nightFlag is derived locally each frame; the host's value is authoritative
        // so day/night-dependent actors (drawbridge, Stalchildren, Poes) flip together.
        remoteNightFlag = (u8)payload.value("night", 0);
        hasRemoteNightFlag = true;
        lastTimeSyncRecvAt = std::chrono::steady_clock::now();
    }
}
