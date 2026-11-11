#ifndef STARSHIP_REPLAYCHECK_H
#define STARSHIP_REPLAYCHECK_H

/*
 * Deterministic mode (netplay groundwork; see docs/netplay-notes.md).
 * SF64_DETERMINISTIC=1 (implied by the SF64_REPLAY_* variables, later):
 *   - Rand_Init seeds from SF64_SEED (default 1) instead of the clock;
 *   - the Versus time limit counts game ticks (100/30 centiseconds each) instead of real time;
 *   - the boot logo waits a fixed 30 ticks instead of one real second.
 */

#include <libultraship/libultra/controller.h>

#ifdef __cplusplus
extern "C" {
#endif

int ReplayCheck_Active(void);
/* seeds for Rand_Init (each 1..29999) */
void ReplayCheck_GetSeeds(int* s1, int* s2, int* s3);
/* centiseconds the Versus timer advances in this tick */
int ReplayCheck_VsTimerCentis(void);
/* SF64_REPLAY_CHECK=<file> per-tick state hash, SF64_REPLAY_RECORD/PLAY=<file> pads, SF64_REPLAY_FRAMES=n quit */
/* right after osContGetReadData in Controller_ReadData */
void ReplayCheck_FilterPads(OSContPad* pads);
/* after Game_Update */
void ReplayCheck_EndTick(void);

#ifdef __cplusplus
}
#endif

#endif
