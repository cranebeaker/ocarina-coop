#ifndef PARTYSETTINGSH
#define PARTYSETTINGSH

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
    };
};
#endif
