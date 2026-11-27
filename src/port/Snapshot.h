#ifndef STARSHIP_SNAPSHOT_H
#define STARSHIP_SNAPSHOT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Whole-simulation snapshots: the __SFSNAP segment (game C globals) + the used part of sMemoryBuffer. */
int Snapshot_Supported(void);
void Snapshot_Init(int slots);
/* the most one slot can need (globals + the whole memory pool) */
size_t Snapshot_MaxBytes(void);
/* slots take equal shares of this read/write block (i64's game memory arena), else malloc;
 * NULL: the block is going away, drop every slot */
void Snapshot_SetStorage(void* base, size_t size);
void Snapshot_Save(int slot);
int Snapshot_Load(int slot);
size_t Snapshot_Size(int slot);
double Snapshot_LastSaveMs(void);
double Snapshot_LastLoadMs(void);
/* resync (n64net_resync_*): the state in a form another device can load (pointers translated, n64net_reloc.h);
 * size 0 = not available. Load returns 0 if refused (another build). */
size_t Snapshot_ResyncSize(void);
void Snapshot_ResyncSave(unsigned char* out);
int Snapshot_ResyncLoad(const unsigned char* in);
/* number of differing ranges between two slots; a short report */
int Snapshot_Compare(int a, int b, char* report, size_t len);
#ifdef __cplusplus
}
#endif
#endif
