#ifndef PARTYSETTINGSH
#define PARTYSETTINGSH

#include <string>

namespace ZeldaOnline {
    struct PartySettings {
        bool privateDungeons = false;
        bool extraEnemies = false;
        float extraEnemyWeight = 1.0f;
        bool healthMultiplier = false;
        float enemyHealthWeight = 1.0f;
        float bossHealthWeight = 1.0f;
        bool storyCoop = false;
        bool shareAmmoRupeesHp = false;
        // Optional label that picks the server side team save together with the admin nickname (max 32 characters).
        std::string storyCode;
    };
};
#endif
