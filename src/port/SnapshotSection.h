/* Rollback snapshot section placement (see src/port/Snapshot.c).
 *
 * Force-included (CMake) into the game's C files (src/engine, src/overlays, src/sys, a few others; not src/audio
 * and not the port's own C/C++). It puts their mutable globals, including function-static locals, into the
 * __SFSNAP segment, so a snapshot of the simulation = that segment + the used part of sMemoryBuffer (the game's
 * bump allocator). SF_SNAPSHOT_OUTSIDE moves one global of a snapshotted file back out (render buffers etc.);
 * SF_SNAPSHOT_INSIDE puts port state that is really game state (ReplayCheck's tick counters) in.
 *
 * Apple (Mach-O) only; elsewhere this is a no-op and rollback is unavailable. */
#ifndef SF_SNAPSHOT_SECTION_H
#define SF_SNAPSHOT_SECTION_H

#if defined(__APPLE__) && defined(__clang__)
#define SF_SNAPSHOT_SUPPORTED 1
#define SF_SNAPSHOT_SEGMENT "__SFSNAP"
#define SF_SNAPSHOT_OUTSIDE __attribute__((section("__DATA,__bss")))
#define SF_SNAPSHOT_OUTSIDE_DATA __attribute__((section("__DATA,__data")))
#define SF_SNAPSHOT_INSIDE __attribute__((section("__SFSNAP,__data")))
#if defined(SF_SNAPSHOT_FORCE) && !defined(__cplusplus)
#pragma clang section bss = "__SFSNAP,__bss" data = "__SFSNAP,__data"
#endif
#else
#define SF_SNAPSHOT_SUPPORTED 0
#define SF_SNAPSHOT_OUTSIDE
#define SF_SNAPSHOT_OUTSIDE_DATA
#define SF_SNAPSHOT_INSIDE
#endif

#endif
