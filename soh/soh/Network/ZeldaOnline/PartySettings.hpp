#ifndef PARTYSETTINGSH
#define PARTYSETTINGSH

#include <string>
#include <random>

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

    // A fresh random team code (8 characters, no look-alike letters). Story Co-op needs one so that two parties whose
    // admins share a nickname never share a server side team save.
    inline std::string GenerateStoryCode() {
        static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
        std::random_device rd;
        std::string code;
        for (int i = 0; i < 8; i++)
            code += alphabet[rd() % (sizeof(alphabet) - 1)];
        return code;
    }
};
#endif
