#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "global.h"
#include "sf64save.h"
#include "ReplayCheck.h"
#include "SfNet.h"
#include "Rollback.h"
#include "Snapshot.h"
#include "n64net_input.h"
#include "n64net_rendezvous.h"
#include "sfnet_build_id.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
#define SFNET_IOS 1
#else
#define SFNET_IOS 0
#endif

/* See SfNet.h. Lockstep (rollback window 0) through n64net_input; the whole game is shared from boot. */

#define SFNET_HS_MAGIC 0x53464e48u /* 'SFNH' handshake */
#define SFNET_HELLO 1
#define SFNET_WELCOME 2
#define SFNET_MISMATCH 3
#define SFNET_BYE 4
#define SFNET_SAVE_MAGIC 0x53465356u /* 'SFSV' the host's save file */
#define SFNET_SAVE_REQ 1
#define SFNET_SAVE_DATA 2
#define SFNET_SYNC_EVERY 30
#define SFNET_STATS_EVERY 900

static int sWanted = -1;
static int sActive = 0;
static int sOffline = 0;
static int sIsHost = 0;
static int sSock = -1;
static struct sockaddr_in sPeer;
static int sHavePeer = 0;
static n64net_input* sNet = NULL;
static unsigned long long sSeed = 1;
static unsigned long long sConfigHash = 0;
static unsigned sBotSeed = 0;
static int sBotFrom = 0; /* SF64_NET_BOT_FROM=tick: the bot takes over from then (scripted menus before) */
static int sWaitSecs = 60;
static int sDropSecs = 20;
static int sPeerLeft = 0;
static int sPeerAway = 0; /* i64 says the other player's app is in the background */
static volatile int sMismatch = 0;
static int sLastTick = -1;
static OSContPad sTickPads[2];
#define SFNET_PAD_RING 128
static OSContPad sUsedPads[SFNET_PAD_RING][2]; /* the pads each tick used (resimulation after going offline) */
static unsigned long long sStallUsecs = 0;
static unsigned sStalls = 0;
static int sDesyncReported = 0;
static char sError[256];

static SaveFile sHostSave; /* host: its save file; guest: the host's, received (never written here) */
static int sHaveHostSave = 0;

static int (*sExtSend)(const void* data, unsigned size) = NULL;
static int (*sExtRecv)(void* buf, unsigned cap) = NULL;

/* rollback (SF64_NET_ROLLBACK, default on where snapshots work; inside Versus gameplay only, lockstep elsewhere) */
#define SFNET_SLOTS 16
#define SFNET_HASH_RING 64
static int sRollback = 0;
static unsigned sRollbackWindow = 8;
static int sRollbackFrom = 30; /* ticks of rollback-safe state before the window opens */
static int sWindowOpen = 0;
static int sSafeSince = -1;
static unsigned long long sTickHash[SFNET_HASH_RING];
static int sTickHashValid[SFNET_HASH_RING];
static int sLastSyncRecorded = -1;
static unsigned sRollbacks = 0, sRollbackTicks = 0, sMaxDepth = 0, sSkips = 0;

/* test aid: a simulated bad link on sent packets (SF64_NET_SIM_LATENCY/JITTER ms, SF64_NET_SIM_LOSS %) */
#define SIM_QUEUE 1024
typedef struct {
    unsigned long long at;
    unsigned size;
    u8 data[1500];
} SimPacket;
static SimPacket* sSimQueue = NULL;
static int sSimHead = 0, sSimCount = 0;
static unsigned sSimLatency = 0, sSimJitter = 0, sSimLoss = 0, sSimRand = 12345;

static const char* EnvStr(const char* name) {
    const char* v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static unsigned long long NowUsecs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000000ull + (unsigned long long) ts.tv_nsec / 1000ull;
}

static FILE* LogFile(void) {
#if SFNET_IOS
    /* iOS: no console for the player; Documents/sfnet.log (Files app) */
    static FILE* f = NULL;
    static int tried = 0;
    if (!tried) {
        char path[1024];
        tried = 1;
        snprintf(path, sizeof(path), "%s/Documents/sfnet.log", getenv("HOME") ? getenv("HOME") : ".");
        f = fopen(path, "w");
    }
    return f ? f : stderr;
#else
    return stderr;
#endif
}

static void NetLog(const char* fmt, ...) {
    va_list ap;
    FILE* out = LogFile();
    va_start(ap, fmt);
    fprintf(out, "SfNet: ");
    vfprintf(out, fmt, ap);
    va_end(ap);
    if (fmt[0] && fmt[strlen(fmt) - 1] != '\n') {
        fputc('\n', out);
    }
    fflush(out);
}

static void NetLogCb(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    NetLog("%s", buf);
}

static unsigned long long Fnv(unsigned long long h, const void* data, size_t len) {
    const unsigned char* p = data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* ------------------------------------------------------------------ transport */

static void RawSend(const void* data, unsigned size) {
    if (sExtSend) {
        sExtSend(data, size);
    } else if (sSock >= 0 && sHavePeer) {
        sendto(sSock, data, size, 0, (struct sockaddr*) &sPeer, sizeof(sPeer));
    }
}

static unsigned SimRand(void) {
    sSimRand = sSimRand * 1103515245u + 12345u;
    return sSimRand >> 8;
}

static void SimFlush(void) {
    unsigned long long now = NowUsecs();
    int i;
    for (i = 0; i < sSimCount;) {
        SimPacket* p = &sSimQueue[(sSimHead + i) % SIM_QUEUE];
        if (p->at <= now) {
            RawSend(p->data, p->size);
            *p = sSimQueue[sSimHead];
            sSimHead = (sSimHead + 1) % SIM_QUEUE;
            sSimCount--;
        } else {
            i++;
        }
    }
}

static void NetSend(void* user, const uint8_t* data, uint32_t size) {
    SimPacket* p;
    (void) user;
    if (!sSimQueue) {
        RawSend(data, size);
        return;
    }
    if (sSimLoss && SimRand() % 100 < sSimLoss) {
        return;
    }
    if (sSimCount >= SIM_QUEUE || size > sizeof(p->data)) {
        RawSend(data, size);
        return;
    }
    p = &sSimQueue[(sSimHead + sSimCount) % SIM_QUEUE];
    p->at = NowUsecs() + (unsigned long long) sSimLatency * 1000 +
            (sSimJitter ? (unsigned long long) (SimRand() % (sSimJitter * 1000)) : 0);
    p->size = size;
    memcpy(p->data, data, size);
    sSimCount++;
}

static void SendHandshake(u8 type) {
    u8 buf[24];
    u32 magic = htonl(SFNET_HS_MAGIC);
    memcpy(buf, &magic, 4);
    buf[4] = type;
    memcpy(buf + 5, &sConfigHash, 8);
    memcpy(buf + 13, &sSeed, 8); /* WELCOME: the session's seed (the host's) */
    RawSend(buf, 21);
}

static void SendSave(u8 kind) {
    u8 out[8 + sizeof(SaveFile)];
    u32 m = htonl(SFNET_SAVE_MAGIC);
    unsigned len = 8;
    memset(out, 0, 8);
    memcpy(out, &m, 4);
    out[4] = kind;
    if (kind == SFNET_SAVE_DATA) {
        memcpy(out + 8, &sHostSave, sizeof(SaveFile));
        len += sizeof(SaveFile);
    }
    RawSend(out, len);
}

/* Reads every waiting datagram. Returns 1 once the other machine is known to be in the session. */
static int Poll(void) {
    int started = 0;
    u8 buf[1500];
    if (sSimCount) {
        SimFlush();
    }
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n;
        u32 magic;
        memset(&from, 0, sizeof(from));
        if (sExtRecv) {
            n = sExtRecv(buf, sizeof(buf));
        } else if (sSock >= 0) {
            n = recvfrom(sSock, buf, sizeof(buf), 0, (struct sockaddr*) &from, &fromlen);
        } else {
            n = 0;
        }
        if (n <= 0) {
            break;
        }
        if (n >= 8 && (memcpy(&magic, buf, 4), ntohl(magic) == SFNET_SAVE_MAGIC)) {
            if (buf[4] == SFNET_SAVE_REQ && sIsHost) {
                SendSave(SFNET_SAVE_DATA);
            } else if (buf[4] == SFNET_SAVE_DATA && !sIsHost && n >= 8 + (ssize_t) sizeof(SaveFile)) {
                memcpy(&sHostSave, buf + 8, sizeof(SaveFile));
                sHaveHostSave = 1;
            }
            continue;
        }
        if (n >= 21 && (memcpy(&magic, buf, 4), ntohl(magic) == SFNET_HS_MAGIC)) {
            unsigned long long hash;
            memcpy(&hash, buf + 5, 8);
            if (buf[4] == SFNET_BYE) {
                if (sActive && !sPeerLeft) {
                    NetLog("the other player quit");
                }
                sPeerLeft = 1;
            } else if (sIsHost && buf[4] == SFNET_HELLO) {
                if (!sExtSend) {
                    sPeer = from;
                    sHavePeer = 1;
                }
                if (hash != sConfigHash) {
                    SendHandshake(SFNET_MISMATCH);
                    NetLog("the guest has a different build");
                    continue;
                }
                SendHandshake(SFNET_WELCOME);
                started = 1;
            } else if (!sIsHost && buf[4] == SFNET_WELCOME) {
                memcpy(&sSeed, buf + 13, 8);
                started = 1;
            } else if (!sIsHost && buf[4] == SFNET_MISMATCH) {
                sMismatch = 1;
            }
            continue;
        }
        if (sNet && n64net_input_handle_packet(sNet, buf, (uint32_t) n)) {
            started = 1;
        }
    }
    return started;
}

/* ------------------------------------------------------------------ connect */

static int PeerGone(void) {
    if (sPeerLeft) {
        return 1;
    }
    if (n64net_input_peer_away(sNet) || sPeerAway) {
        return 0; /* its app is in the background: wait as long as it takes */
    }
    return n64net_input_peer_silence_usecs(sNet) > (unsigned long long) sDropSecs * 1000000ull;
}

static void GoOffline(void) {
    if (sOffline) {
        return;
    }
    sOffline = 1;
    NetLog("%s Playing on offline.", sPeerLeft ? "The other player left." : "Lost the connection to the other player.");
}

static void CreateNet(void) {
    n64net_input_config cfg;
    const char* s;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_players = 2;
    cfg.local_player = sIsHost ? 0 : 1;
    cfg.session_id = (u32) (sConfigHash ^ (sConfigHash >> 32));
    cfg.input_delay = (s = EnvStr("SF64_NET_DELAY")) ? (u32) atoi(s) : 2;
    cfg.rollback_window = 0; /* lockstep */
    cfg.ticks_per_second = 30;
    cfg.send = NetSend;
    cfg.log = NetLogCb;
    sNet = n64net_input_create(&cfg);
    n64net_input_set_auto_delay(sNet, cfg.input_delay, sRollback ? 3 : 6);
}

/* Waits for the host's save (guest) */
static int GetHostSave(void) {
    unsigned long long deadline = NowUsecs() + (unsigned long long) sWaitSecs * 1000000ull, next = 0;
    while (!sIsHost && !sHaveHostSave) {
        unsigned long long now;
        Poll();
        now = NowUsecs();
        if (now > deadline || sPeerLeft) {
            snprintf(sError, sizeof(sError), "Netplay: didn't receive the host's save data.");
            return -1;
        }
        if (now >= next) {
            SendSave(SFNET_SAVE_REQ);
            next = now + 50000;
        }
        usleep(1000);
    }
    return 0;
}

static void Activate(void) {
    sActive = 1;
    sLastTick = -1;
    if (sRollback) {
        Snapshot_Init(SFNET_SLOTS);
    }
    NetLog("connected as player %d (%s, input delay %u, build %s, seed %llu)", sIsHost ? 1 : 2,
           sRollback ? "rollback in Versus" : "lockstep",
           n64net_input_get_delay(sNet), SFNET_BUILD_ID, sSeed);
}

static int ConnectSocket(void) {
    const char* s;
    const char* server = EnvStr("SF64_NET_SERVER");
    int port = server ? 0 : (sIsHost ? 7401 : 7402);
    struct sockaddr_in local;
    unsigned long long deadline, nextHello = 0;

    if ((s = EnvStr("SF64_NET_PORT"))) {
        port = atoi(s);
    }
    if (!sIsHost && !server) {
        char host[128];
        int peerport = 7401;
        const char* peer = EnvStr("SF64_NET_PEER");
        const char* colon = peer ? strrchr(peer, ':') : NULL;
        if (!peer) {
            snprintf(sError, sizeof(sError), "Netplay: the guest needs SF64_NET_PEER=host:port");
            return -1;
        }
        snprintf(host, sizeof(host), "%.*s", colon ? (int) (colon - peer) : (int) strlen(peer), peer);
        if (colon) {
            peerport = atoi(colon + 1);
        }
        memset(&sPeer, 0, sizeof(sPeer));
        sPeer.sin_family = AF_INET;
        sPeer.sin_port = htons((u16) peerport);
        if (inet_pton(AF_INET, host, &sPeer.sin_addr) != 1) {
            snprintf(sError, sizeof(sError), "Netplay: SF64_NET_PEER must be an IPv4 address:port");
            return -1;
        }
        sHavePeer = 1;
    }

    sSock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((u16) port);
    if (sSock < 0 || bind(sSock, (struct sockaddr*) &local, sizeof(local)) < 0) {
        snprintf(sError, sizeof(sError), "Netplay: can't use UDP port %d: %s", port, strerror(errno));
        return -1;
    }
    fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL, 0) | O_NONBLOCK);

    if (server) {
        char game[96];
        N64NetRendezvousConfig rc;
        N64NetRendezvousResult res;
        snprintf(game, sizeof(game), "sf64-us+%s", SFNET_BUILD_ID);
        memset(&rc, 0, sizeof(rc));
        memset(&res, 0, sizeof(res));
        rc.server = server;
        rc.game = game;
        rc.room = EnvStr("SF64_NET_ROOM");
        rc.password = EnvStr("SF64_NET_PASSWORD");
        rc.is_host = sIsHost;
        rc.force_relay = EnvStr("SF64_NET_RELAY") && atoi(EnvStr("SF64_NET_RELAY"));
        rc.wait_secs = sWaitSecs;
        rc.log = NetLogCb;
        if (n64net_rendezvous_connect(sSock, &rc, &res) != 0) {
            snprintf(sError, sizeof(sError), "Netplay: couldn't connect through the room server (%s).", res.error);
            return -1;
        }
        sPeer = res.peer;
        sHavePeer = 1;
        NetLog("room %s, %s connection", res.room, res.relayed ? "relayed" : "direct");
    }

    CreateNet();
    NetLog("%s on UDP %d, waiting up to %d s", sIsHost ? "host" : "guest", port, sWaitSecs);
    deadline = NowUsecs() + (unsigned long long) sWaitSecs * 1000000ull;
    for (;;) {
        unsigned long long now;
        if (Poll()) {
            break;
        }
        if (sMismatch) {
            snprintf(sError, sizeof(sError), "Netplay: the other player has a different build of the game.");
            return -1;
        }
        now = NowUsecs();
        if (now > deadline) {
            snprintf(sError, sizeof(sError), "Netplay: the other player didn't connect within %d seconds.", sWaitSecs);
            return -1;
        }
        if (!sIsHost && now >= nextHello) {
            SendHandshake(SFNET_HELLO);
            nextHello = now + 100000;
        }
        usleep(1000);
    }
    return GetHostSave();
}

static void ReadEnvCommon(void) {
    const char* s;
    if ((s = EnvStr("SF64_NET_BOT"))) sBotSeed = (unsigned) strtoul(s, NULL, 0) | 1;
    if ((s = EnvStr("SF64_NET_BOT_FROM"))) sBotFrom = atoi(s);
    if ((s = EnvStr("SF64_NET_WAIT_SECS"))) sWaitSecs = atoi(s);
    if ((s = EnvStr("SF64_NET_DROP_SECS"))) sDropSecs = atoi(s) > 0 ? atoi(s) : 20;
    sRollback = !(EnvStr("SF64_NET_ROLLBACK") && atoi(EnvStr("SF64_NET_ROLLBACK")) == 0) && Snapshot_Supported();
    if ((s = EnvStr("SF64_NET_ROLLBACK_WINDOW"))) sRollbackWindow = (unsigned) atoi(s);
    if ((s = EnvStr("SF64_NET_ROLLBACK_FROM"))) sRollbackFrom = atoi(s);
    if ((s = EnvStr("SF64_NET_SIM_LATENCY"))) sSimLatency = (unsigned) atoi(s);
    if ((s = EnvStr("SF64_NET_SIM_JITTER"))) sSimJitter = (unsigned) atoi(s);
    if ((s = EnvStr("SF64_NET_SIM_LOSS"))) sSimLoss = (unsigned) atoi(s);
    if ((sSimLatency || sSimJitter || sSimLoss) && !sSimQueue) {
        sSimQueue = calloc(SIM_QUEUE, sizeof(SimPacket));
        sSimRand ^= (unsigned) getpid();
        NetLog("simulating %u ms + %u ms jitter, %u%% loss on sent packets", sSimLatency, sSimJitter, sSimLoss);
    }
}

/* test aid SF64_NET_EXTERNAL_TEST=1: stands in for i64 (SfNet_InitExternal) with a plain UDP socket as the game
 * channel (host 7401, guest 7402 -> 127.0.0.1:7401) and the session blob through a file (SF64_NET_EXTERNAL_BLOB) */
static int sTestSock = -1;
static struct sockaddr_in sTestPeer;
static int sTestHavePeer = 0;

static int TestSend(const void* data, unsigned size) {
    return sTestHavePeer ? (int) sendto(sTestSock, data, size, 0, (struct sockaddr*) &sTestPeer, sizeof(sTestPeer)) : -1;
}

static int TestRecv(void* buf, unsigned cap) {
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    ssize_t n = recvfrom(sTestSock, buf, cap, 0, (struct sockaddr*) &from, &len);
    if (n > 0 && !sTestHavePeer) {
        sTestPeer = from;
        sTestHavePeer = 1;
    }
    return n > 0 ? (int) n : 0;
}

static void ExternalTest(unsigned long long seed) {
    int host = EnvStr("SF64_NET_HOST") && atoi(EnvStr("SF64_NET_HOST"));
    const char* path = EnvStr("SF64_NET_EXTERNAL_BLOB");
    static u8 blob[4096];
    unsigned blobsize = 0;
    struct sockaddr_in local;
    SfNetExternal x;
    int i;

    sTestSock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(host ? 7401 : 7402);
    bind(sTestSock, (struct sockaddr*) &local, sizeof(local));
    fcntl(sTestSock, F_SETFL, fcntl(sTestSock, F_GETFL, 0) | O_NONBLOCK);
    if (host) {
        FILE* f;
        blobsize = SfNet_SessionBlob(blob, sizeof(blob));
        f = path ? fopen(path, "wb") : NULL;
        if (f) {
            fwrite(blob, 1, blobsize, f);
            fclose(f);
        }
    } else {
        memset(&sTestPeer, 0, sizeof(sTestPeer));
        sTestPeer.sin_family = AF_INET;
        sTestPeer.sin_port = htons(7401);
        inet_pton(AF_INET, "127.0.0.1", &sTestPeer.sin_addr);
        sTestHavePeer = 1;
        for (i = 0; i < 200 && !blobsize; i++) {
            FILE* f = path ? fopen(path, "rb") : NULL;
            if (f) {
                blobsize = (unsigned) fread(blob, 1, sizeof(blob), f);
                fclose(f);
            }
            if (!blobsize) usleep(50000);
        }
        for (i = 0; i < 3; i++) TestSend("hi", 2); /* the host learns the address (i64 knows it already) */
    }
    memset(&x, 0, sizeof(x));
    x.localplayer = host ? 0 : 1;
    x.seed = seed;
    x.blob = blob;
    x.blobsize = blobsize;
    x.send = TestSend;
    x.recv = TestRecv;
    if (SfNet_InitExternal(&x) != 0) {
        NetLog("%s", sError);
        exit(1);
    }
}

/* Settings file for machines without environment variables (iPhone: Documents/netplay.cfg; elsewhere
 * SF64_NET_CONFIG=<file>): one KEY=VALUE per line (SF64_* keys only, '#' comments). The environment wins. */
static void LoadConfigFile(void) {
    char path[1024], line[512];
    FILE* f;
    int n = 0;
    if (EnvStr("SF64_NET_CONFIG")) {
        snprintf(path, sizeof(path), "%s", EnvStr("SF64_NET_CONFIG"));
    } else if (SFNET_IOS) {
        snprintf(path, sizeof(path), "%s/Documents/netplay.cfg", getenv("HOME") ? getenv("HOME") : ".");
    } else {
        return;
    }
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char* eq;
        size_t len;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "SF64_", 5) != 0 || !(eq = strchr(p, '='))) {
            continue;
        }
        *eq = '\0';
        len = strlen(eq + 1);
        while (len && (eq[len] == '\n' || eq[len] == '\r' || eq[len] == ' ')) eq[len--] = '\0';
        setenv(p, eq + 1, 0);
        n++;
    }
    fclose(f);
    NetLog("%d settings from %s", n, path);
}

int SfNet_Wanted(void) {
    if (sWanted < 0) {
        LoadConfigFile();
        sWanted = EnvStr("SF64_NET_HOST") || EnvStr("SF64_NET_PEER") ||
                  (EnvStr("SF64_NET_SERVER") && EnvStr("SF64_NET_ROOM")) || EnvStr("SF64_NET_EXTERNAL_TEST");
    }
    return sWanted;
}

unsigned long long SfNet_Init(unsigned long long seed) {
    if (!SfNet_Wanted() || sActive) {
        return seed;
    }
    sSeed = seed;
    ReadEnvCommon();
    if (EnvStr("SF64_NET_EXTERNAL_TEST")) {
        ExternalTest(seed);
        return sSeed;
    }
    sIsHost = EnvStr("SF64_NET_HOST") && atoi(EnvStr("SF64_NET_HOST"));
    if (sIsHost) {
        Save_ReadEeprom(&sHostSave); /* the save the guest will play on */
        sHaveHostSave = 1;
    }
    /* everything that must be equal on both machines (the seed comes from the host) */
    sConfigHash = Fnv(0xcbf29ce484222325ull, SFNET_BUILD_ID, strlen(SFNET_BUILD_ID));
    if (ConnectSocket() != 0) {
        NetLog("%s", sError);
        exit(1);
    }
    Activate();
    return sSeed;
}

/* ------------------------------------------------------------------ host app (i64) */

#define SFNET_BLOB_MAGIC 0x53465342u /* 'SFSB' */

unsigned SfNet_SessionBlob(void* out, unsigned cap) {
    u32 h[2] = { SFNET_BLOB_MAGIC, 1 };
    SaveFile save;
    if (sizeof(h) + sizeof(SaveFile) > cap) {
        return 0;
    }
    Save_ReadEeprom(&save);
    memcpy(out, h, sizeof(h));
    memcpy((u8*) out + sizeof(h), &save, sizeof(SaveFile));
    return (unsigned) (sizeof(h) + sizeof(SaveFile));
}

int SfNet_InitExternal(const SfNetExternal* x) {
    const u32* h = x->blob;
    ReadEnvCommon();
    sExtSend = x->send;
    sExtRecv = x->recv;
    sIsHost = x->localplayer == 0;
    sSeed = x->seed;
    sWanted = 1;
    if (!h || x->blobsize < 8 + sizeof(SaveFile) || h[0] != SFNET_BLOB_MAGIC || h[1] != 1) {
        snprintf(sError, sizeof(sError), "Netplay: the host's session data is missing or from another version.");
        return -1;
    }
    memcpy(&sHostSave, (const u8*) x->blob + 8, sizeof(SaveFile));
    sHaveHostSave = 1;
    sConfigHash = Fnv(0xcbf29ce484222325ull, &sSeed, sizeof(sSeed));
    CreateNet();
    Activate();
    NetLog("running on the host app's connection");
    return 0;
}

void SfNet_ExternalEvent(int event) {
    if (event == SFNET_PEER_AWAY) {
        sPeerAway = 1;
    } else if (event == SFNET_PEER_BACK) {
        sPeerAway = 0;
    } else if (event == SFNET_PEER_LEFT) {
        sPeerLeft = 1;
    }
}

/* ------------------------------------------------------------------ per tick */

int SfNet_Active(void) {
    return sActive && !sOffline;
}

int SfNet_InSession(void) {
    return sActive; /* stays after a disconnect (the match keeps its two players) */
}

int SfNet_LocalPlayer(void) {
    return sIsHost ? 0 : 1;
}

static n64net_pad ToNet(const OSContPad* p) {
    n64net_pad n;
    memset(&n, 0, sizeof(n));
    n.buttons = p->button;
    n.stick_x = p->stick_x;
    n.stick_y = p->stick_y;
    n.rstick_x = p->right_stick_x;
    n.rstick_y = p->right_stick_y;
    return n;
}

static void FromNet(const n64net_pad* n, OSContPad* p) {
    memset(p, 0, sizeof(*p));
    p->button = n->buttons;
    p->stick_x = n->stick_x;
    p->stick_y = n->stick_y;
    p->right_stick_x = n->rstick_x;
    p->right_stick_y = n->rstick_y;
}

static void ExchangeTick(int tick, const OSContPad* local) {
    const int me = sIsHost ? 0 : 1;
    int p;
    if (sOffline) {
        sTickPads[me] = *local;
        memset(&sTickPads[1 - me], 0, sizeof(sTickPads[0]));
        return;
    }
    n64net_input_set_local(sNet, (u32) tick, ToNet(local));
    if (!n64net_input_ready(sNet, (u32) tick)) {
        unsigned long long start = NowUsecs(), lastCheck = start;
        sStalls++;
        while (!n64net_input_ready(sNet, (u32) tick)) {
            unsigned long long now;
            Poll();
            n64net_input_resend(sNet);
            now = NowUsecs();
            if (sPeerLeft || now - lastCheck > 1000000) {
                lastCheck = now;
                if (PeerGone()) {
                    sStallUsecs += now - start;
                    GoOffline();
                    ExchangeTick(tick, local);
                    return;
                }
            }
            usleep(250);
        }
        sStallUsecs += NowUsecs() - start;
    }
    for (p = 0; p < 2; p++) {
        n64net_pad n;
        n64net_input_get(sNet, p, (u32) tick, &n);
        FromNet(&n, &sTickPads[p]);
    }
}

void SfNet_FilterPads(OSContPad* pads, int tick) {
    int i;
    if (!sActive) {
        return;
    }
    if (gRollbackResim && sOffline) {
        sTickPads[0] = sUsedPads[tick % SFNET_PAD_RING][0];
        sTickPads[1] = sUsedPads[tick % SFNET_PAD_RING][1];
    } else if (gRollbackResim) {
        /* resimulation: the ledger's inputs for that tick (confirmed, or the newest prediction) */
        int p;
        for (p = 0; p < 2; p++) {
            n64net_pad n;
            n64net_input_get(sNet, p, (u32) tick, &n);
            FromNet(&n, &sTickPads[p]);
        }
    } else if (!sOffline) {
        Poll();
    }
    if (!gRollbackResim && tick != sLastTick) {
        OSContPad local = pads[0]; /* this machine's own controller is always controller 1 */
        int t;
        if (sBotSeed && tick >= sBotFrom) {
            ReplayCheck_BotPad(tick, sBotSeed, &local);
        }
        /* ticks in which the game read no pads still get an input (unchanged): the ledgers stay in step */
        for (t = sLastTick + 1; t <= tick; t++) {
            ExchangeTick(t, &local);
            sUsedPads[t % SFNET_PAD_RING][0] = sTickPads[0];
            sUsedPads[t % SFNET_PAD_RING][1] = sTickPads[1];
        }
        sLastTick = tick;
    }
    {
        static int traceFrom = -2, traceTo = -1;
        if (traceFrom == -2) {
            const char* e = EnvStr("SF64_NET_TRACE");
            traceFrom = -1;
            if (e) sscanf(e, "%d:%d", &traceFrom, &traceTo);
        }
        if (tick >= traceFrom && tick <= traceTo) {
            NetLog("trace tick %d: local %04x, used P1 %04x P2 %04x", tick, pads[0].button, sTickPads[0].button,
                   sTickPads[1].button);
        }
    }
    for (i = 0; i < 4; i++) {
        if (i < 2) {
            pads[i] = sTickPads[i];
            pads[i].err_no = 0;
        } else {
            memset(&pads[i], 0, sizeof(pads[i]));
            pads[i].err_no = 8; /* CONT_NO_RESPONSE_ERROR: unplugged */
        }
    }
}

void SfNet_EndTick(int tick, unsigned long long hash) {
    u32 dtick;
    u8 dkind;
    if (!sActive || sOffline) {
        return;
    }
    Poll();
    if (PeerGone()) {
        GoOffline();
        return;
    }
    {
        static int desyncAt = -2;
        if (desyncAt == -2) {
            desyncAt = EnvStr("SF64_NET_TEST_DESYNC") ? atoi(EnvStr("SF64_NET_TEST_DESYNC")) : -1;
        }
        if (tick == desyncAt) {
            extern s32 sRandSeed1;
            sRandSeed1 = sRandSeed1 % 29000 + 1 + (sRandSeed1 == 1);
            NetLog("test desync injected at tick %d", tick);
        }
    }
    /* every tick is confirmed in lockstep; with rollback the rollback point records confirmed ticks */
    if (!sRollback && tick % SFNET_SYNC_EVERY == 0) {
        n64net_sync_record(sNet, (u32) tick, 0, (u32) (hash ^ (hash >> 32)));
    }
    if (!sDesyncReported && n64net_sync_desynced(sNet, &dtick, &dkind)) {
        sDesyncReported = 1;
        NetLog("DESYNC detected at tick %u", dtick);
    }
    if (tick % SFNET_STATS_EVERY == 0 && tick > 0) {
        n64net_input_stats st;
        n64net_input_get_stats(sNet, &st);
        NetLog("tick %d, sync checks matched %u%s, stalls %u (%.2f s), delay %u, rtt avg %u ms max %u ms, "
               "rollbacks %u (%u ticks, max %u), skips %u, save %.2f ms", tick,
               n64net_sync_matched(sNet), sDesyncReported ? " (DESYNC)" : "", sStalls, (double) sStallUsecs / 1e6,
               n64net_input_get_delay(sNet), st.rtt_avg_usecs / 1000, st.rtt_max_usecs / 1000, sRollbacks,
               sRollbackTicks, sMaxDepth, sSkips, Snapshot_LastSaveMs());
    }
}

int SfNet_SaveRead(void* saveFile) {
    if (!sActive || sIsHost || !sHaveHostSave) {
        return 0;
    }
    memcpy(saveFile, &sHostSave, sizeof(SaveFile)); /* the guest plays on the host's save */
    return 1;
}

int SfNet_SaveWriteBlocked(void) {
    return sActive && !sIsHost; /* never overwrite the guest's own save with the host's */
}

void SfNet_Shutdown(void) {
    int i;
    if (sActive && !sOffline) {
        for (i = 0; i < 3; i++) {
            SendHandshake(SFNET_BYE);
        }
    }
}

int SfNet_FullView(int camCount) {
    static int split = -1;
    if (split < 0) {
        const char* v = EnvStr("SF64_NET_VIEW");
        split = v && !strcmp(v, "split");
    }
    return camCount == 4 && sActive && !split; /* stays after a disconnect: the view doesn't jump */
}

/* ------------------------------------------------------------------ rollback */

int SfNet_RollbackActive(void) {
    return sActive && !sOffline && sRollback;
}

static void CbSave(void* user, uint32_t slot) {
    (void) user;
    Snapshot_Save((int) slot);
}

static void CbLoad(void* user, uint32_t slot) {
    (void) user;
    Snapshot_Load((int) slot);
    ReplayCheck_SetTick((int) n64net_rollback_callback_tick(sNet)); /* the state before that tick */
}

static void CbAdvance(void* user) {
    int t;
    (void) user;
    Rollback_Step();
    t = ReplayCheck_GetTick();
    sTickHash[t % SFNET_HASH_RING] = ReplayCheck_StateHash();
    sTickHashValid[t % SFNET_HASH_RING] = t;
}

/* all inputs up to tick - 1 confirmed (the ones the current state was built from) */
static void WaitConfirmed(int tick) {
    while (tick > 0 && !sOffline) {
        uint32_t c = n64net_input_confirmed_tick(sNet);
        if (c != UINT32_MAX && (int) c >= tick - 1) {
            break;
        }
        Poll();
        n64net_input_resend(sNet);
        if (PeerGone()) {
            GoOffline();
        }
        usleep(250);
    }
}

void SfNet_RollbackPoint(void) {
    const int tick = ReplayCheck_GetTick(); /* the state now = before tick `tick` */
    const int safe = Rollback_Safe();
    int want, closing = 0, depth, t;
    n64net_rollback_callbacks cb;
    uint32_t confirmed;

    if (!safe) {
        sSafeSince = -1;
    } else if (sSafeSince < 0) {
        sSafeSince = tick;
    }
    want = safe && tick >= sSafeSince + sRollbackFrom;
    if (sWindowOpen && !want) {
        /* leaving rollback-safe play (a state change is pending): settle every prediction first; the correction
         * below may even cancel the change */
        WaitConfirmed(tick);
        closing = 1;
    } else if (!sWindowOpen && want) {
        n64net_input_set_window(sNet, sRollbackWindow);
        sWindowOpen = 1;
    }

    memset(&cb, 0, sizeof(cb));
    cb.save = CbSave;
    cb.load = CbLoad;
    cb.advance = CbAdvance;
    cb.slot_count = SFNET_SLOTS;
    gRollbackResim = 1;
    depth = n64net_rollback_frame(sNet, (uint32_t) tick, &cb);
    gRollbackResim = 0;

    if (closing) {
        n64net_input_set_window(sNet, 0);
        sWindowOpen = 0;
    }
    if (depth > 0) {
        sRollbacks++;
        sRollbackTicks += (unsigned) depth;
        if ((unsigned) depth > sMaxDepth) sMaxDepth = (unsigned) depth;
    } else if (depth < 0) {
        NetLog("rollback needed a state older than the snapshot ring");
    }

    sTickHash[tick % SFNET_HASH_RING] = ReplayCheck_StateHash();
    sTickHashValid[tick % SFNET_HASH_RING] = tick;

    /* sync checks on confirmed states: the state before tick t is final once inputs up to t - 1 are confirmed */
    confirmed = n64net_input_confirmed_tick(sNet);
    if (confirmed != UINT32_MAX) {
        /* the state after tick c is final once inputs up to c are confirmed */
        ReplayCheck_TickFinal((int) confirmed < tick - 1 ? (int) confirmed : tick - 1);
        for (t = sLastSyncRecorded + 1; t <= (int) confirmed + 1 && t <= tick; t++) {
            if (t % SFNET_SYNC_EVERY == 0 && sTickHashValid[t % SFNET_HASH_RING] == t) {
                unsigned long long h = sTickHash[t % SFNET_HASH_RING];
                n64net_sync_record(sNet, (u32) t, 0, (u32) (h ^ (h >> 32)));
            }
            sLastSyncRecorded = t;
        }
    }
}

int SfNet_TimeSyncSkip(void) {
    if (!sActive || sOffline || !sRollback || !sWindowOpen) {
        return 0;
    }
    Poll();
    if (n64net_timesync_should_skip(sNet)) {
        sSkips++;
        return 1;
    }
    return 0;
}
