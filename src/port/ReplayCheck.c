#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "ReplayCheck.h"
#include "SfNet.h"
#include "Rollback.h"
#include "SnapshotSection.h"

/* See ReplayCheck.h. */

static int sInit = 0;
static int sActive = 0;
static unsigned long long sSeed = 1;
/* game state, rolled back with the game (Versus timer, boot logo wait) */
SF_SNAPSHOT_INSIDE static int sVsTimerCalls = 0;
SF_SNAPSHOT_INSIDE static int sLogoWaitTicks = 0;
static int sTick = 0; // logic ticks since boot
static int sMaxTicks = -1;
static FILE* sHashLog = NULL;
static FILE* sRecord = NULL;
/* the hash log is written once a tick is final: at once, or (rollback) when its inputs are confirmed */
#define LINE_RING 256
static char sLines[LINE_RING][160];
static int sLineTick[LINE_RING];
static int sWritten = -1;
#define REPLAY_PADS 4
typedef struct {
    u16 button;
    s8 x, y, rx, ry;
    u8 err;
} ReplayPad;
static ReplayPad (*sPlay)[REPLAY_PADS] = NULL;
static int sPlayCount = 0;
static unsigned sBotSeed[REPLAY_PADS]; /* SF64_TEST_BOT / _BOT2..4: scripted input per controller (0 = off) */

extern s32 sRandSeed1, sRandSeed2, sRandSeed3, sSeededRandSeed1, sSeededRandSeed2, sSeededRandSeed3;

static void LoadPlayback(const char* path) {
    FILE* f = fopen(path, "r");
    char line[512];
    int cap = 0;
    if (!f) {
        fprintf(stderr, "ReplayCheck: can't open %s\n", path);
        exit(1);
    }
    while (fgets(line, sizeof(line), f)) {
        int tick, n, used;
        const char* p = line;
        ReplayPad fr[REPLAY_PADS];
        if (line[0] == '#' || sscanf(p, "%d%n", &tick, &used) != 1 || tick < 0) {
            continue;
        }
        p += used;
        memset(fr, 0, sizeof(fr));
        for (n = 0; n < REPLAY_PADS; n++) {
            unsigned b;
            int x, y, rx, ry, err;
            if (sscanf(p, "%x %d %d %d %d %d%n", &b, &x, &y, &rx, &ry, &err, &used) != 6) {
                break;
            }
            p += used;
            fr[n].button = b; fr[n].x = x; fr[n].y = y; fr[n].rx = rx; fr[n].ry = ry; fr[n].err = err;
        }
        if (n != REPLAY_PADS) {
            continue;
        }
        if (tick >= cap) {
            int nc = cap ? cap * 2 : 4096;
            while (nc <= tick) nc *= 2;
            sPlay = realloc(sPlay, nc * sizeof(*sPlay));
            memset(sPlay + cap, 0, (nc - cap) * sizeof(*sPlay));
            cap = nc;
        }
        memcpy(sPlay[tick], fr, sizeof(fr));
        if (tick + 1 > sPlayCount) sPlayCount = tick + 1;
    }
    fclose(f);
}

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
    if ((s = EnvStr("SF64_REPLAY_FRAMES"))) {
        sMaxTicks = atoi(s);
    }
    if ((s = EnvStr("SF64_REPLAY_CHECK"))) {
        sHashLog = fopen(s, "w");
        if (sHashLog) fprintf(sHashLog, "# starship replay check: seed %llu\n", sSeed);
    }
    if ((s = EnvStr("SF64_REPLAY_RECORD"))) {
        sRecord = fopen(s, "w");
        if (sRecord) fprintf(sRecord, "# starship replay input: tick then 4 x (buttons sx sy rsx rsy err)\n");
    }
    sBotSeed[0] = (s = EnvStr("SF64_TEST_BOT")) ? ((unsigned) strtoul(s, NULL, 0) | 1) : 0;
    {
        int i;
        char name[32];
        for (i = 1; i < REPLAY_PADS; i++) {
            snprintf(name, sizeof(name), "SF64_TEST_BOT%d", i + 1);
            sBotSeed[i] = (s = EnvStr(name)) ? ((unsigned) strtoul(s, NULL, 0) | 1) : 0;
        }
    }
    if (sBotSeed[0] || sBotSeed[1]) {
        sActive = 1;
    }
    if (SfNet_Wanted()) {
        sActive = 1;
        sSeed = SfNet_Init(sSeed); /* connects; the host's seed wins */
        atexit(SfNet_Shutdown);
    }
    if ((s = EnvStr("SF64_REPLAY_PLAY"))) {
        LoadPlayback(s);
        if (sMaxTicks < 0) sMaxTicks = sPlayCount + 30;
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

/* Scripted test input: a new random choice every 8 ticks (stick, aim, fire, bombs, boost/brake, menus) */
void ReplayCheck_BotPad(int tick, unsigned seed, OSContPad* pad) {
    unsigned x = seed ^ ((unsigned) (tick / 8) * 2654435761u);
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    pad->button = 0;
    pad->stick_x = (s8) ((int) (x % 141) - 70);
    pad->stick_y = (s8) ((int) ((x >> 8) % 141) - 70);
    pad->right_stick_x = 0;
    pad->right_stick_y = 0;
    pad->err_no = 0;
    if ((x >> 16) & 1) pad->button |= A_BUTTON;           /* fire / menu accept */
    if (((x >> 17) & 7) == 0) pad->button |= B_BUTTON;    /* bomb / back */
    if (((x >> 20) & 7) == 0) pad->button |= Z_TRIG;
    if (((x >> 23) & 7) == 0) pad->button |= R_TRIG;
    if (((x >> 26) & 7) == 0) pad->button |= U_CBUTTONS;  /* boost */
    if (((x >> 29) & 3) == 0 && (tick % 8) < 2) pad->button |= START_BUTTON;
}

void ReplayCheck_FilterPads(OSContPad* pads) {
    int i;
    if (!ReplayCheck_Active()) {
        return;
    }
    for (i = 0; i < REPLAY_PADS; i++) {
        if (sBotSeed[i]) {
            ReplayCheck_BotPad(sTick, sBotSeed[i], &pads[i]);
        }
    }
    if (sPlay) {
        for (i = 0; i < REPLAY_PADS; i++) {
            ReplayPad* r = sTick < sPlayCount ? &sPlay[sTick][i] : NULL;
            pads[i].button = r ? r->button : 0;
            pads[i].stick_x = r ? r->x : 0;
            pads[i].stick_y = r ? r->y : 0;
            pads[i].right_stick_x = r ? r->rx : 0;
            pads[i].right_stick_y = r ? r->ry : 0;
            pads[i].err_no = r ? r->err : 0;
            pads[i].gyro_x = pads[i].gyro_y = 0;
        }
        SfNet_FilterPads(pads, sTick); /* online: the played pad 1 is this machine's player */
        return;
    }
    for (i = 0; i < REPLAY_PADS; i++) {
        pads[i].gyro_x = pads[i].gyro_y = 0; // not recorded
    }
    if (sRecord) {
        fprintf(sRecord, "%d", sTick);
        for (i = 0; i < REPLAY_PADS; i++) {
            fprintf(sRecord, " %x %d %d %d %d %d", pads[i].button, pads[i].stick_x, pads[i].stick_y,
                    pads[i].right_stick_x, pads[i].right_stick_y, pads[i].err_no);
        }
        fputc('\n', sRecord);
    }
    SfNet_FilterPads(pads, sTick); /* online: pad 1 (after the bot) is this machine's player */
}

/* FNV-1a 64 */
static unsigned long long HashBytes(unsigned long long h, const void* data, size_t len) {
    const unsigned char* p = data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
#define HASHV(h, v) h = HashBytes(h, &(v), sizeof(v))

static unsigned long long HashObj(unsigned long long h, Object* o) {
    HASHV(h, o->status);
    HASHV(h, o->id);
    HASHV(h, o->pos);
    HASHV(h, o->rot);
    return h;
}

static unsigned long long HashState(int* numObjs) {
    unsigned long long h = 0xcbf29ce484222325ull;
    int i, n = 0;
    HASHV(h, gGameState);
    HASHV(h, gGameFrameCount);
    HASHV(h, sRandSeed1); HASHV(h, sRandSeed2); HASHV(h, sRandSeed3);
    HASHV(h, sSeededRandSeed1); HASHV(h, sSeededRandSeed2); HASHV(h, sSeededRandSeed3);
    if (gVersusMode) {
        HASHV(h, gVsPoints); h = HashBytes(h, gVsCountdown, 3 * sizeof(s32)); HASHV(h, gPlayerInactive);
    }
    if (gPlayer != NULL) {
        for (i = 0; i < gCamCount && i < 4; i++) {
            Player* p = &gPlayer[i];
            HASHV(h, p->pos); HASHV(h, p->vel); HASHV(h, p->rot); HASHV(h, p->trueZpos);
            HASHV(h, p->state); HASHV(h, p->form); HASHV(h, p->shields); HASHV(h, p->damage);
        }
    }
    for (i = 0; i < ARRAY_COUNT(gActors); i++) {
        if (gActors[i].obj.status != OBJ_FREE) {
            h = HashObj(h, &gActors[i].obj); HASHV(h, gActors[i].state); HASHV(h, gActors[i].health); n++;
        }
    }
    for (i = 0; i < ARRAY_COUNT(gBosses); i++) {
        if (gBosses[i].obj.status != OBJ_FREE) {
            h = HashObj(h, &gBosses[i].obj); HASHV(h, gBosses[i].state); HASHV(h, gBosses[i].health); n++;
        }
    }
    for (i = 0; i < ARRAY_COUNT(gItems); i++) {
        if (gItems[i].obj.status != OBJ_FREE) {
            h = HashObj(h, &gItems[i].obj); HASHV(h, gItems[i].state); n++;
        }
    }
    *numObjs = n;
    return h;
}

void ReplayCheck_TickFinal(int tick) {
    int t;
    if (!sHashLog) {
        return;
    }
    for (t = sWritten + 1; t <= tick; t++) {
        if (sLineTick[t % LINE_RING] == t) {
            fputs(sLines[t % LINE_RING], sHashLog);
        }
    }
    if (tick > sWritten) {
        sWritten = tick;
    }
}

unsigned long long ReplayCheck_StateHash(void) {
    int n;
    return HashState(&n);
}

int ReplayCheck_GetTick(void) {
    return sTick;
}

void ReplayCheck_SetTick(int tick) {
    sTick = tick;
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

    if (sHashLog) {
        int n;
        unsigned long long h = HashState(&n);
        snprintf(sLines[sTick % LINE_RING], sizeof(sLines[0]), "%d state %d frame %d objs %d rng %d %d %d hash %016llx\n",
                 sTick, gGameState, gGameFrameCount, n, sRandSeed1, sRandSeed2, sRandSeed3, h);
        sLineTick[sTick % LINE_RING] = sTick;
    }

    if (gRollbackResim) {
        sTick++; /* resimulated tick: no netplay, no quitting */
        return;
    }

    if (SfNet_Active()) {
        int n;
        SfNet_EndTick(sTick, HashState(&n));
    }

    if (!SfNet_RollbackActive()) {
        ReplayCheck_TickFinal(sTick);
    }

    sTick++;
    if (sMaxTicks >= 0 && sTick >= sMaxTicks) {
        if (sHashLog) fclose(sHashLog);
        if (sRecord) fclose(sRecord);
        sHashLog = sRecord = NULL;
        fprintf(stderr, "ReplayCheck: %d ticks done, quitting\n", sTick);
        SfNet_Shutdown();
        _Exit(0);
    }
}
