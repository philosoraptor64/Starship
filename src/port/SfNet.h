#ifndef STARSHIP_SFNET_H
#define STARSHIP_SFNET_H

/*
 * 2-player netplay over n64net (external/n64net): lockstep, the whole game shared from boot like two controllers
 * on one console (host = controller 1, guest = controller 2; each machine's own pad is its controller 1). The guest
 * plays on the host's save and never writes its own. See docs/netplay-notes.md.
 *
 * Standalone (environment):
 *   SF64_NET_HOST=1 | SF64_NET_PEER=ip:port   direct UDP (host 7401, guest 7402; SF64_NET_PORT)
 *   SF64_NET_SERVER=host:port SF64_NET_ROOM=ABCD [SF64_NET_PASSWORD, SF64_NET_RELAY=1]   room codes
 *   SF64_NET_DELAY=n (2; auto up to 6), SF64_NET_WAIT_SECS (60), SF64_NET_DROP_SECS (20: then play on offline)
 *   SF64_NET_BOT=seed (scripted local input), SF64_NET_TEST_DESYNC=tick, SF64_NET_EXTERNAL_TEST=1 (i64 stand-in)
 *   The host's SF64_SEED is the session's. On the iPhone the same KEY=VALUE lines go in Documents/netplay.cfg
 *   (elsewhere SF64_NET_CONFIG=<file>); the log is Documents/sfnet.log there.
 * Inside i64: SfNet_SessionBlob on the session host, SfNet_InitExternal at game start, SfNet_ExternalEvent per frame.
 */

#include <libultraship/libultra/controller.h>

#ifdef __cplusplus
extern "C" {
#endif

int SfNet_Wanted(void);
/* connects (blocks); returns the session's seed (the host's) */
unsigned long long SfNet_Init(unsigned long long seed);
int SfNet_Active(void);
/* 1 from connecting until the game quits, also after the other player left (SfNet_Active: connected now) */
int SfNet_InSession(void);
int SfNet_LocalPlayer(void); /* this machine's first player */
/* online Versus (4 cameras): draw only this machine's own pilots (SF64_NET_VIEW=split: classic 4-way).
 * SfNet_FullView = SfNet_ViewCount > 0. One pilot fills the screen; 2-4 take quarter-size views
 * (SfNet_ViewRect, N64 screen units). SfNet_ViewOf = a pilot's view index, -1 if not shown here. */
int SfNet_FullView(int camCount);
int SfNet_ViewCount(int camCount);
int SfNet_ViewPilot(int view);
/* online Versus with one local pilot on this device: that pilot (sounds are placed from its camera,
   as in single player); -1 otherwise (the game's own Versus sound: every source centred) */
int SfNet_SfxPilot(int camCount);
int SfNet_ViewOf(int pilot);
void SfNet_ViewRect(int view, float* x, float* y, float* w, float* h);
/* right after osContGetReadData (and the replay check): pads 1-2 = the exchanged inputs of logic tick `tick` */
void SfNet_FilterPads(OSContPad* pads, int tick);
/* after each logic tick, with the state hash */
void SfNet_EndTick(int tick, unsigned long long hash);
/* guest: the host's save instead of its own (1 = filled); writes are skipped */
int SfNet_SaveRead(void* saveFile);
int SfNet_SaveWriteBlocked(void);
void SfNet_Shutdown(void);
/* every frame: 1 while this app is in the background (iOS); the other player waits without a limit */
void SfNet_SetAway(int away);
/* rollback netplay (online Versus): the rollback point after each tick */
int SfNet_RollbackActive(void);
void SfNet_RollbackPoint(void);
/* once per frame before the tick: 1 = this side is ahead, skip this frame's tick (time sync) */
int SfNet_TimeSyncSkip(void);

/* A host app (i64) owns the connection and starts the game fresh on every machine. */
typedef struct SfNetExternal {
    int localplayer;              /* 0 = the session's host (player 1), 1 = guest */
    unsigned long long seed;      /* the same on every machine */
    const void* blob;             /* the host's SfNet_SessionBlob, delivered reliably */
    unsigned blobsize;
    int (*send)(const void* data, unsigned size);
    int (*recv)(void* buf, unsigned cap); /* one datagram: size, 0 if none */
    int (*send_to)(unsigned machine, const void* data, unsigned size); /* one machine only (a joiner's log) */
    /* i64's player table (ABI v2); num_players 0 = two players, one per machine (localplayer) */
    int num_players;              /* 2-4 player slots in the session */
    unsigned local_mask;          /* the players this machine controls (bit p) */
    int pad_of_player[4];         /* this machine's controller index for each local player */
    int machine_of_player[4];     /* which machine plays each player (0 = the session's host) */
    int dropout_menu;             /* the host app asks the session's host what to do when a player leaves */
    int joining;                  /* this machine joins the running game (catches up from the host's log) */
} SfNetExternal;
enum { SFNET_PEER_AWAY = 1, SFNET_PEER_BACK = 2, SFNET_PEER_LEFT = 3 }; /* = n64_net_event */
unsigned SfNet_SessionBlob(void* out, unsigned cap);
int SfNet_InitExternal(const SfNetExternal* x);
void SfNet_ExternalEvent(int event);
/* the same with the machine it concerns (-1 = unknown) */
void SfNet_ExternalEventFrom(int event, int machine);
/* joining mid-game (i64 n64_game_catching_up / n64_game_catch_up_tick) */
int SfNet_JoinPending(void);                        /* 1 from the joining start until live (once) */
int SfNet_CatchUpReady(void);                       /* the next tick's inputs are here */
void SfNet_CatchUpProgress(unsigned* done, unsigned* total);
extern int gSfNetCatchUp;                           /* a catch-up tick: no sound */
/* the session's host: a newcomer takes over `machine`'s pilots (left, idle now). 1 = admitted */
int SfNet_Admit(int machine);
/* drop-outs, on the session's host: action 2 = remove the pilot from the match, 3 = it stays idle
 * (n64_dropout_action); 0 = done, else not possible */
int SfNet_Dropout(int player, int action);
/* inside a host app: why this side ends the match (a desync, the friend gone); "" = keep going */
const char* SfNet_EndReason(void);

#ifdef __cplusplus
}
#endif

#endif
