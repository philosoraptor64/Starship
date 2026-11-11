#include <stdlib.h>
#include "global.h"
#include "ReplayCheck.h"

/* See ReplayCheck.h. */

static int sInit = 0;
static int sActive = 0;
static unsigned long long sSeed = 1;
static int sVsTimerCalls = 0;
static int sLogoWaitTicks = 0;

static const char* EnvStr(const char* name) {
    const char* v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static void Init(void) {
    const char* s;

    if (sInit) {
        return;
    }
    sInit = 1;
    sActive = ((s = EnvStr("SF64_DETERMINISTIC")) && atoi(s)) || EnvStr("SF64_REPLAY_CHECK") ||
              EnvStr("SF64_REPLAY_RECORD") || EnvStr("SF64_REPLAY_PLAY");
    if ((s = EnvStr("SF64_SEED"))) {
        sSeed = strtoull(s, NULL, 0);
    }
}

int ReplayCheck_Active(void) {
    Init();
    return sActive;
}

void ReplayCheck_GetSeeds(int* s1, int* s2, int* s3) {
    Init();
    *s1 = 1 + (int) (sSeed % 29999);
    *s2 = 1 + (int) ((sSeed * 7919 + 13) % 29999);
    *s3 = 1 + (int) ((sSeed * 104729 + 29) % 29999);
}

int ReplayCheck_VsTimerCentis(void) {
    /* 30 logic ticks per second: 100 centiseconds spread over 30 ticks without drift */
    int prev = sVsTimerCalls * 100 / 30;
    sVsTimerCalls++;
    return sVsTimerCalls * 100 / 30 - prev;
}

void ReplayCheck_EndTick(void) {
    if (!ReplayCheck_Active()) {
        return;
    }

    /* boot logo: a fixed 30 ticks (the original waits one real second on a timer task) */
    if (gGameState == GSTATE_LOGO_WAIT) {
        if (++sLogoWaitTicks >= 30) {
            gGameState++;
        }
    } else {
        sLogoWaitTicks = 0;
    }
}
