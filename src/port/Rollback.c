#include <stdlib.h>
#include <stdio.h>
#include "global.h"
#include "Rollback.h"
#include "Snapshot.h"
#include "ReplayCheck.h"
#include "SfNet.h"

/*
 * SF64_SYNCTEST=N (+ SF64_SYNCTEST_FROM=tick, default 60; SF64_SYNCTEST_ALWAYS=1 = also outside rollback-safe
 * states): after every tick, rewind N ticks, resimulate them and compare the state hash (and with
 * SF64_SYNCTEST_FULL=1 the whole snapshot) with the original. 0 mismatches = rollback-safe.
 */

int gRollbackResim = 0;

void Main_LogicTick(bool resim);

#define HASH_RING 64

static int sInit = 0;
static int sSyncTest = 0, sFrom = 60, sAlways = 0, sFull = 0, sSlots = 0;
static unsigned long long sHash[HASH_RING];
static unsigned sChecks = 0, sMismatches = 0;

void Rollback_Step(void) {
    Main_LogicTick(true);
}

int Rollback_Safe(void) {
    return gVersusMode && (gGameState == GSTATE_PLAY) && (gNextGameState == GSTATE_NONE);
}

static void Init(void) {
    const char* s;
    if (sInit) {
        return;
    }
    sInit = 1;
    if ((s = getenv("SF64_SYNCTEST")) && *s && Snapshot_Supported()) {
        sSyncTest = atoi(s);
        if (sSyncTest < 1) sSyncTest = 1;
        if (sSyncTest > HASH_RING - 2) sSyncTest = HASH_RING - 2;
        sFrom = (s = getenv("SF64_SYNCTEST_FROM")) ? atoi(s) : 60;
        sAlways = (s = getenv("SF64_SYNCTEST_ALWAYS")) && atoi(s);
        sFull = (s = getenv("SF64_SYNCTEST_FULL")) && atoi(s);
        sSlots = sSyncTest + 2;
        Snapshot_Init(sSlots);
        fprintf(stderr, "SyncTest: rewinding %d ticks every tick from tick %d%s\n", sSyncTest, sFrom,
                sAlways ? " (everywhere)" : " (in Versus gameplay)");
    }
}

static void SyncTestPoint(void) {
    const int tick = ReplayCheck_GetTick(); /* the state now = the state before tick `tick` */
    int from = tick - sSyncTest, t;
    static int safeSince = -1;
    unsigned long long h;

    sHash[tick % HASH_RING] = ReplayCheck_StateHash();
    Snapshot_Save(tick % sSlots);

    /* only rewind into states that were all rollback-safe */
    if (!sAlways) {
        if (!Rollback_Safe()) {
            safeSince = -1;
            return;
        }
        if (safeSince < 0) {
            safeSince = tick;
        }
        if (from < safeSince) {
            return;
        }
    }
    if (from < sFrom || !Snapshot_Load(from % sSlots)) {
        return;
    }
    ReplayCheck_SetTick(from);
    gRollbackResim = 1;
    for (t = from; t < tick; t++) {
        Rollback_Step();
    }
    gRollbackResim = 0;
    h = ReplayCheck_StateHash();
    sChecks++;
    if (sFull) {
        char report[1024];
        int scratch = (tick + 1) % sSlots;
        static int reported = 0;
        Snapshot_Save(scratch);
        if (Snapshot_Compare(tick % sSlots, scratch, report, sizeof(report)) && reported++ < 20) {
            fprintf(stderr, "SyncTest: state differs before tick %d:%s\n", tick, report);
        }
    }
    if (h != sHash[tick % HASH_RING] || ReplayCheck_GetTick() != tick) {
        if (sMismatches++ < 10) {
            fprintf(stderr, "SyncTest: MISMATCH before tick %d (rewound %d)\n", tick, sSyncTest);
        }
    }
    if (tick % 300 == 0) {
        fprintf(stderr, "SyncTest: tick %d, %u checks, %u mismatches, snapshot %zu KB, save %.2f ms, load %.2f ms\n",
                tick, sChecks, sMismatches, Snapshot_Size(tick % sSlots) / 1024, Snapshot_LastSaveMs(),
                Snapshot_LastLoadMs());
    }
}

void Rollback_AfterTick(void) {
    Init();
    if (sSyncTest) {
        SyncTestPoint();
    } else if (SfNet_RollbackActive()) {
        SfNet_RollbackPoint();
    }
}
