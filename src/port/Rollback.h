#ifndef STARSHIP_ROLLBACK_H
#define STARSHIP_ROLLBACK_H
#ifdef __cplusplus
extern "C" {
#endif
/* Set while a tick is resimulated (rollback): no sound, no rumble, nothing submitted. */
extern int gRollbackResim;
/* After every real tick (Graphics_ThreadUpdate): SyncTest / the netplay rollback point. */
void Rollback_AfterTick(void);
/* Resimulates one logic tick (Main_LogicTick(true)). */
void Rollback_Step(void);
/* Rollback is allowed now: online Versus gameplay (no radio voices, no state change pending). */
int Rollback_Safe(void);
#ifdef __cplusplus
}
#endif
#endif
