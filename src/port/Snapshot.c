#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "SnapshotSection.h"
#include "Snapshot.h"

#if SF_SNAPSHOT_SUPPORTED
#include "n64net_reloc.h"
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
    int owned; /* buf from malloc (else from the storage block) */
    Range ranges[3];
    int numRanges, valid;
} Slot;

static Range sData, sBss;
static Slot* sSlots = NULL;
static int sNumSlots = 0;
static double sSaveMs = 0, sLoadMs = 0;
static unsigned char* sStore = NULL; /* Snapshot_SetStorage: i64's game memory arena */
static size_t sStoreSize = 0;

static double NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int Snapshot_Supported(void) {
    return 1;
}

static void FindSections(void) {
    unsigned long size = 0;
    sData.base = getsectiondata(&__dso_handle, SF_SNAPSHOT_SEGMENT, "__data", &size);
    sData.size = sData.base ? size : 0;
    size = 0;
    sBss.base = getsectiondata(&__dso_handle, SF_SNAPSHOT_SEGMENT, "__bss", &size);
    sBss.size = sBss.base ? size : 0;
}

size_t Snapshot_MaxBytes(void) {
    FindSections();
    return sData.size + sBss.size + 1000000 * sizeof(int); /* + all of sMemoryBuffer */
}

void Snapshot_SetStorage(void* base, size_t size) {
    sStore = (unsigned char*) base;
    sStoreSize = base ? size : 0;
    if (!base) { /* the block goes away (i64 wipes it): forget slots that point into it */
        int i;
        for (i = 0; i < sNumSlots; i++) {
            if (sSlots[i].owned) free(sSlots[i].buf);
        }
        free(sSlots);
        sSlots = NULL;
        sNumSlots = 0;
    }
}

void Snapshot_Init(int slots) {
    int i;
    FindSections();
    if (sSlots) {
        for (i = 0; i < sNumSlots; i++) {
            if (sSlots[i].owned) free(sSlots[i].buf);
        }
    }
    free(sSlots);
    sSlots = calloc(slots, sizeof(Slot));
    sNumSlots = slots;
    if (sStore && slots > 0) { /* equal shares of the block */
        size_t share = (sStoreSize / (size_t) slots) & ~(size_t) 15;
        for (i = 0; i < slots; i++) {
            sSlots[i].buf = sStore + (size_t) i * share;
            sSlots[i].cap = share;
        }
    }
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
        if (s->owned) free(s->buf);
        s->cap = total + total / 4;
        s->buf = malloc(s->cap);
        s->owned = 1;
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

/* ---- resync (another device's state, n64net_reloc.h): the same three ranges, pointers translated ---- */

static n64net_reloc sReloc;
static int sRelocReady = 0;

static int ResyncSpans(n64net_reloc_span* s) {
    FindSections();
    if (!sRelocReady) {
        sRelocReady = n64net_reloc_init(&sReloc, &__dso_handle) ? 1 : -1;
        /* sMemoryBuffer lies in the image: its pointers go as image offsets. i64's game memory arena is at one
         * fixed address on every device: pointers into it mean the same there. */
        if (sRelocReady > 0) {
            n64net_reloc_add(&sReloc, (const void*) (uintptr_t) 0x110000000ULL, (size_t) (576ULL << 20));
        }
    }
    s[0].base = sData.base;
    s[0].used = s[0].cap = sData.size;
    s[1].base = sBss.base;
    s[1].used = s[1].cap = sBss.size;
    s[2].base = sMemoryBuffer;
    s[2].used = (size_t) ((unsigned char*) sMemoryPtr - (unsigned char*) sMemoryBuffer);
    s[2].cap = 1000000 * sizeof(int);
    return sRelocReady > 0 && sData.base && sBss.base ? 3 : 0;
}

size_t Snapshot_ResyncSize(void) {
    n64net_reloc_span s[3];
    return ResyncSpans(s) ? n64net_reloc_state_size(s, 3) : 0;
}

void Snapshot_ResyncSave(unsigned char* out) {
    n64net_reloc_span s[3];
    n64net_reloc_stats st;
    double t0 = NowMs();
    if (!ResyncSpans(s)) return;
    n64net_reloc_scan(&sReloc);
    n64net_reloc_save(&sReloc, s, 3, out, &st);
    fprintf(stderr, "Snapshot: resync state %zu KB: %llu words in the game's image, %llu elsewhere in memory, "
            "%llu escaped, %.1f ms\n", (s[0].used + s[1].used + s[2].used) / 1024,
            (unsigned long long) st.region_ptrs, (unsigned long long) st.foreign, (unsigned long long) st.escaped,
            NowMs() - t0);
    if (getenv("SF64_NET_RELOC_DUMP")) { /* test: <dir>/sf-resync-<pid>.bin, to compare two devices' states */
        char path[1024];
        FILE* f;
        snprintf(path, sizeof(path), "%s/sf-resync-%d.bin", getenv("SF64_NET_RELOC_DUMP"), (int) getpid());
        if ((f = fopen(path, "wb"))) {
            fwrite(out, 1, n64net_reloc_state_size(s, 3), f);
            fclose(f);
            fprintf(stderr, "Snapshot: resync state written to %s\n", path);
        }
    }
}

int Snapshot_ResyncLoad(const unsigned char* in) {
    n64net_reloc_span s[3];
    n64net_reloc_stats st;
    int ok;
    if (!ResyncSpans(s)) return 0;
    n64net_reloc_scan(&sReloc);
    ok = n64net_reloc_load(&sReloc, s, 3, in, NULL, &st);
    fprintf(stderr, "Snapshot: resync load %s: %llu words kept, %llu translated, %llu own pointers, %llu matched, "
            "%llu guessed\n", ok ? "done" : "REFUSED (another build?)", (unsigned long long) st.same,
            (unsigned long long) st.translated, (unsigned long long) st.own_pointer, (unsigned long long) st.matched,
            (unsigned long long) st.guessed);
    return ok;
}

#else
size_t Snapshot_ResyncSize(void) { return 0; }
void Snapshot_ResyncSave(unsigned char* out) { (void) out; }
int Snapshot_ResyncLoad(const unsigned char* in) { (void) in; return 0; }
int Snapshot_Supported(void) { return 0; }
void Snapshot_Init(int slots) { (void) slots; }
size_t Snapshot_MaxBytes(void) { return 0; }
void Snapshot_SetStorage(void* base, size_t size) { (void) base; (void) size; }
void Snapshot_Save(int slot) { (void) slot; }
int Snapshot_Load(int slot) { (void) slot; return 0; }
size_t Snapshot_Size(int slot) { (void) slot; return 0; }
double Snapshot_LastSaveMs(void) { return 0; }
double Snapshot_LastLoadMs(void) { return 0; }
int Snapshot_Compare(int a, int b, char* r, size_t n) { (void) a; (void) b; if (n) r[0] = 0; return 0; }
#endif
