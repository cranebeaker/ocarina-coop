#ifndef Z_BG_DDAN_KD_H
#define Z_BG_DDAN_KD_H

#include <libultraship/libultra.h>
#include "global.h"

struct BgDdanKd;

typedef void (*BgDdanKdActionFunc)(struct BgDdanKd*, PlayState*);

typedef struct BgDdanKd {
    /* 0x0000 */ DynaPolyActor dyna;
    /* 0x0164 */ Actor* prevExplosive;
    /* 0x0168 */ s16 timer;
    /* 0x016C */ Vec3f prevExplosivePos;
    /* 0x0178 */ ColliderCylinder collider;
    /* 0x01C4 */ BgDdanKdActionFunc actionFunc;
} BgDdanKd; // size = 0x01C8

// Anchor co-op helpers (only called while connected)
s32 BgDdanKd_IsWaiting(BgDdanKd* self);
void BgDdanKd_ForceLower(BgDdanKd* self, PlayState* play);
void BgDdanKd_ForceSnapLowered(BgDdanKd* self, PlayState* play);

#endif
