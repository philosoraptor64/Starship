#include "sys.h"
#include "port/SnapshotSection.h"

// s32 sMemoryBuffer[0x8800];
// @ port: Increase memory buffer size.
// (outside the snapshot section: Snapshot.c copies only its used part, up to sMemoryPtr)
SF_SNAPSHOT_OUTSIDE s32 sMemoryBuffer[1000000];
s32* sMemoryPtr;

void Memory_FreeAll(void) {
    sMemoryPtr = sMemoryBuffer;
}

void* Memory_Allocate(s32 size) {
    void* addr = sMemoryPtr;

    sMemoryPtr = (void*) (((size + 0xF) & ~0xF) + (uintptr_t) sMemoryPtr);
    return addr;
}
