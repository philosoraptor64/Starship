#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "SnapshotSection.h"
#include "Snapshot.h"

#if SF_SNAPSHOT_SUPPORTED
#include <mach-o/getsect.h>
/* this image: the app, or the i64 module dylib (_mh_execute_header is the app only) */
extern const struct mach_header_64 __dso_handle;
#include <mach-o/ldsyms.h>

extern int sMemoryBuffer[];
extern int* sMemoryPtr;

typedef struct {
    unsigned char* base;
    size_t size;
} Range;

typedef struct {
    unsigned char* buf;
    size_t cap, used;
    Range ranges[3];
    int numRanges, valid;
} Slot;

static Range sData, sBss;
static Slot* sSlots = NULL;
static int sNumSlots = 0;
static double sSaveMs = 0, sLoadMs = 0;

static double NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int Snapshot_Supported(void) {
    return 1;
}

void Snapshot_Init(int slots) {
    unsigned long size = 0;
    sData.base = getsectiondata(&__dso_handle, SF_SNAPSHOT_SEGMENT, "__data", &size);
    sData.size = sData.base ? size : 0;
    size = 0;
    sBss.base = getsectiondata(&__dso_handle, SF_SNAPSHOT_SEGMENT, "__bss", &size);
    sBss.size = sBss.base ? size : 0;
    free(sSlots);
    sSlots = calloc(slots, sizeof(Slot));
    sNumSlots = slots;
    fprintf(stderr, "Snapshot: %d slots, globals %zu + %zu bytes\n", slots, sData.size, sBss.size);
}

void Snapshot_Save(int slot) {
    Slot* s;
    Range r[3];
    size_t total = 0;
    int i, n = 0;
    unsigned char* p;
    double t0 = NowMs();
    if (!sSlots || slot < 0) {
        return;
    }
    s = &sSlots[slot % sNumSlots];
    r[n++] = sData;
    r[n++] = sBss;
    r[n].base = (unsigned char*) sMemoryBuffer;
    r[n].size = (size_t) ((unsigned char*) sMemoryPtr - (unsigned char*) sMemoryBuffer);
    n++;
    for (i = 0; i < n; i++) total += r[i].size;
    if (total > s->cap) {
        free(s->buf);
        s->cap = total + total / 4;
        s->buf = malloc(s->cap);
    }
    p = s->buf;
    for (i = 0; i < n; i++) {
        memcpy(p, r[i].base, r[i].size);
        p += r[i].size;
        s->ranges[i] = r[i];
    }
    s->numRanges = n;
    s->used = total;
    s->valid = 1;
    sSaveMs = NowMs() - t0;
}

int Snapshot_Load(int slot) {
    Slot* s;
    const unsigned char* p;
    int i;
    double t0 = NowMs();
    if (!sSlots || slot < 0) {
        return 0;
    }
    s = &sSlots[slot % sNumSlots];
    if (!s->valid) {
        return 0;
    }
    p = s->buf;
    for (i = 0; i < s->numRanges; i++) {
        memcpy(s->ranges[i].base, p, s->ranges[i].size);
        p += s->ranges[i].size;
    }
    sLoadMs = NowMs() - t0;
    return 1;
}

size_t Snapshot_Size(int slot) {
    return sSlots && slot >= 0 ? sSlots[slot % sNumSlots].used : 0;
}

double Snapshot_LastSaveMs(void) {
    return sSaveMs;
}

double Snapshot_LastLoadMs(void) {
    return sLoadMs;
}

int Snapshot_Compare(int a, int b, char* report, size_t len) {
    Slot* sa = &sSlots[a % sNumSlots];
    Slot* sb = &sSlots[b % sNumSlots];
    const unsigned char *pa = sa->buf, *pb = sb->buf;
    size_t pos = 0;
    int i, diffs = 0;
    report[0] = '\0';
    for (i = 0; i < sa->numRanges && i < sb->numRanges; i++) {
        size_t n = sa->ranges[i].size < sb->ranges[i].size ? sa->ranges[i].size : sb->ranges[i].size;
        size_t k, count = 0;
        int spots = 0;
        for (k = 0; k < n; k++) {
            if (pa[k] != pb[k]) {
                count++;
                if (spots < 6 && pos < len) {
                    pos += snprintf(report + pos, len - pos, " r%d+0x%zx", i, k);
                    spots++;
                    k += 15;
                }
            }
        }
        if (count || sa->ranges[i].size != sb->ranges[i].size) {
            diffs++;
            if (pos < len) {
                pos += snprintf(report + pos, len - pos, " [range %d: %zu bytes differ, sizes %zu/%zu]", i, count,
                                sa->ranges[i].size, sb->ranges[i].size);
            }
        }
        pa += sa->ranges[i].size;
        pb += sb->ranges[i].size;
    }
    return diffs;
}

#else
int Snapshot_Supported(void) { return 0; }
void Snapshot_Init(int slots) { (void) slots; }
void Snapshot_Save(int slot) { (void) slot; }
int Snapshot_Load(int slot) { (void) slot; return 0; }
size_t Snapshot_Size(int slot) { (void) slot; return 0; }
double Snapshot_LastSaveMs(void) { return 0; }
double Snapshot_LastLoadMs(void) { return 0; }
int Snapshot_Compare(int a, int b, char* r, size_t n) { (void) a; (void) b; if (n) r[0] = 0; return 0; }
#endif
