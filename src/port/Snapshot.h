#ifndef STARSHIP_SNAPSHOT_H
#define STARSHIP_SNAPSHOT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Whole-simulation snapshots: the __SFSNAP segment (game C globals) + the used part of sMemoryBuffer. */
int Snapshot_Supported(void);
void Snapshot_Init(int slots);
void Snapshot_Save(int slot);
int Snapshot_Load(int slot);
size_t Snapshot_Size(int slot);
double Snapshot_LastSaveMs(void);
double Snapshot_LastLoadMs(void);
/* number of differing ranges between two slots; a short report */
int Snapshot_Compare(int a, int b, char* report, size_t len);
#ifdef __cplusplus
}
#endif
#endif
