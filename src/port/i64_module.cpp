/* Star Fox 64 as an i64 game module (src/port/i64/n64_game.h).
 *
 * Built only with -DSTARSHIP_I64_MODULE=ON, which makes the game a shared
 * library that exports just the n64_game_* functions (CMakeLists.txt,
 * src/port/i64/exports.txt). i64 owns the app, SDL, the window, the audio
 * output and the network session; this file maps its calls onto the
 * stand-alone game's run (Game.cpp: Port_RunBegin / Port_RunFrame /
 * Port_RunEnd) and puts the module back to a clean slate after each run
 * (Reentry.cpp), so it can be started again in the same process. Same layout
 * as SpaghettiKart's and BattleShip's i64_module.cpp.
 *
 * Online runs use the host's game channel (SfNet_InitExternal): the session
 * host's save travels in the session blob (SfNet_SessionBlob), everyone
 * starts from boot with the same seed, Versus is online.
 *
 * ROM import: Torch (linked for the in-game extractor anyway) turns the
 * player's ROM into <data>/sf64.o2r with the recipe in the module's resources
 * (config.yml, assets/yaml/); starship.o2r, the port's own assets, ships there too
 * and is copied next to it.
 */
#if defined(STARSHIP_I64_MODULE)

#define N64_GAME_BUILDING_MODULE 1
#include <atomic>
#include "Snapshot.h"
#include "i64/n64_game.h"

#include "Reentry.h"
#include "sfnet_build_id.h"
#include "sf64_asset_recipe.h"

#include "Companion.h"
#include <sys/mman.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include "port/SfNet.h"
void Port_RunBegin(void);
extern "C" void GameEngine_PreloadAudio(void); /* Engine.cpp */
int Port_RunFrame(void);
void Port_RunEnd(int fullShutdown);
void LUS_SetExternalWindow(void* sdlWindow, void** rendererCache);
void LUS_SetExternalPacing(bool hostPaced);
void GameEngine_SetOnePicturePerHostFrame(bool on);   // Engine.cpp
int GameEngine_HostFrameWithoutLogic(void);
void LUS_SetExternalAudio(bool external);
/* Game audio goes to the host (Engine.cpp) */
extern void (*gPortHostAudioWrite)(const int16_t* stereo, uint32_t frames, uint32_t rate);
extern uint32_t (*gPortHostAudioBuffered)(uint32_t rate);
}

namespace {

const n64_game_rom kRoms[] = {
    { "09f0d105f476b00efa5303a3ebc42e60a7753b7a", "USA 1.1" },
};

const n64_game_info kInfo = {
    N64_GAME_ABI_VERSION,
    "sf64-us",
    "Star Fox 64",
    kRoms,
    1,
    SFNET_BUILD_ID,
    2,
    60,
    0,
};

/* Survive the reset of the globals between runs (Reentry.cpp). */
#define KEEP __attribute__((section("__DATA,__sfkeep")))
KEEP const n64_host* sHost = nullptr;
extern "C" bool gLusHostPaced; /* libultraship gfx_sdl2.cpp */
/* libultraship gfx_metal.cpp: a picture of the screen for i64's home screen (30 s in, then each minute) */
extern "C" void (*gLusFrameCaptureCallback)(const uint8_t* rgba, uint32_t w, uint32_t h);
extern "C" std::atomic<int> gLusFrameCaptureRequest;
static unsigned sPreviewFrames = 0;
static constexpr unsigned kPreviewFirst = 1800, kPreviewEvery = 3600; /* host frames (60/s) */

/* when to offer a picture: kPreviewFirst, then every kPreviewEvery frames; tests: I64_TEST_PREVIEW_AT="400,700" */
static bool PreviewDue(unsigned frame) {
    static const char* at = getenv("I64_TEST_PREVIEW_AT");
    if (at) {
        for (const char* p = at; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr) {
            if ((unsigned) atoi(p) == frame) return true;
        }
        return false;
    }
    return frame >= kPreviewFirst && (frame - kPreviewFirst) % kPreviewEvery == 0;
}
KEEP bool sRunning = false;
KEEP void* sRenderer = nullptr; /* SDL renderer on the host's window, reused across runs */
KEEP void* sRendererWindow = nullptr;
KEEP bool sOnline = false;
KEEP bool sPeerLeft = false;
KEEP const n64_host* sNetHost = nullptr;

int HostSend(const void* data, unsigned size) {
    return sNetHost != nullptr ? sNetHost->net_send(data, size) : -1;
}
int HostRecv(void* buf, unsigned cap) {
    return sNetHost != nullptr ? sNetHost->net_recv(buf, cap) : 0;
}

/* the host's session events, once per frame */
void PollEvents(void) {
    if (sHost == nullptr || sHost->net_poll_event == nullptr) {
        return;
    }
    for (int e; (e = sHost->net_poll_event()) != 0;) {
        if (sOnline) {
            SfNet_ExternalEvent(e);
        }
        if (e == N64_NET_PEER_LEFT) {
            sPeerLeft = true;
        }
    }
}

bool SetDataHome(const n64_host* host) {
    if (host->data_dir == nullptr || host->data_dir[0] == '\0') {
        return false;
    }
    setenv("SHIP_HOME", host->data_dir, 1); /* config, saves (default.sav), logs, sf64.o2r, starship.o2r */
    return true;
}

/* online start-up: the host's session blob (its save) over the game channel */
enum { kPhaseNone, kPhaseExchange, kPhaseGame };
int sPhase = kPhaseNone;
int sPlayer = 0;
unsigned long long sSeed = 0;
uint32_t sRun = 0;
std::vector<uint8_t> sBlob;
uint32_t sExchangeFrames = 0;
constexpr uint32_t kBlobMagic = 0x5346424C; /* 'SFBL' */

void Put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
uint32_t Get32(const uint8_t* p) { return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3]; }

bool BeginGame(void) {
    if (sOnline) {
        SfNetExternal x = {};
        x.localplayer = sPlayer;
        x.seed = sSeed;
        x.blob = sBlob.data();
        x.blobsize = (unsigned) sBlob.size();
        x.send = HostSend;
        x.recv = HostRecv;
        if (sHost->log != nullptr) {
            sHost->log("sf64: online as player %d (session data %zu bytes)", sPlayer + 1, sBlob.size());
        }
        if (SfNet_InitExternal(&x) != 0) {
            return false;
        }
    }
    Port_RunBegin();
    /* the audio thread loads a sequence and its sound bank synchronously when music first starts, and the game
     * thread waits for it (an ~50-110 ms hitch half a second in): load all audio (~10 MB) on LUS's workers now */
    GameEngine_PreloadAudio();
    sPhase = kPhaseGame;
    return true;
}

void SendBlobPacket(uint8_t type) {
    std::vector<uint8_t> pkt(9 + (type == 1 ? sBlob.size() : 0));
    Put32(pkt.data(), kBlobMagic);
    pkt[4] = type;
    Put32(pkt.data() + 5, sRun);
    if (type == 1) {
        std::memcpy(pkt.data() + 9, sBlob.data(), sBlob.size());
    }
    sHost->net_send(pkt.data(), (uint32_t) pkt.size());
}

/* one frame of the exchange: false = give up */
bool ExchangeFrame(void) {
    sExchangeFrames++;
    if (sExchangeFrames > 60 * 60) {
        if (sHost->log != nullptr) {
            sHost->log("sf64: no session data from the other player after a minute");
        }
        return false;
    }
    if (sPlayer == 0 && sExchangeFrames % 6 == 1) {
        SendBlobPacket(1);
    }
    uint8_t buf[2048];
    int n;
    while ((n = sHost->net_recv(buf, sizeof(buf))) > 0) {
        const bool ours = n >= 9 && Get32(buf) == kBlobMagic && Get32(buf + 5) == sRun;
        if (sPlayer == 0) {
            /* the guest's acknowledgement, or its game already talking: it has the blob */
            if (!ours || buf[4] == 2) {
                return BeginGame();
            }
        } else if (ours && buf[4] == 1) {
            sBlob.assign(buf + 9, buf + n);
            for (int i = 0; i < 3; i++) {
                SendBlobPacket(2);
            }
            return BeginGame();
        }
    }
    return true;
}

} // namespace

extern "C" N64_GAME_EXPORT const n64_game_info* n64_game_get_info(void) {
    return &kInfo;
}

extern "C" N64_GAME_EXPORT int n64_game_import_rom(const n64_host* host, const char* rom_path,
                                                   void (*progress)(float, void*), void* user, char* err,
                                                   size_t err_size) {
    namespace fs = std::filesystem;
    /* Before Torch runs: everything so far is static initialisation, the
     * pristine state (else Torch's changes to globals stay after a run) */
    Reentry_SavePristine();
    if (host == nullptr || rom_path == nullptr || host->data_dir == nullptr || host->bundle_dir == nullptr) {
        std::snprintf(err, err_size, "missing folders");
        return 1;
    }
    if (sRunning) {
        std::snprintf(err, err_size, "the game is running");
        return 1;
    }
    if (progress != nullptr) {
        progress(0.0f, user);
    }
    std::error_code ec;
    const fs::path data(host->data_dir);
    const fs::path recipe(host->bundle_dir);
    fs::create_directories(data, ec);
    if (!fs::exists(recipe / "config.yml", ec) || !fs::exists(recipe / "assets" / "yaml", ec)) {
        std::snprintf(err, err_size, "the game's extraction recipe is missing from the module");
        return 1;
    }

    std::vector<uint8_t> rom;
    {
        std::ifstream in(rom_path, std::ios::binary);
        rom.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (rom.size() < (8u << 20) || rom.size() > (64u << 20) || (rom.size() % 4) != 0) {
        std::snprintf(err, err_size, "that file isn't an N64 ROM");
        return 1;
    }
    /* Torch wants big-endian (.z64): 80 37 12 40 */
    if (!(rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)) {
        if (rom[0] == 0x37 && rom[1] == 0x80) { /* .v64: 16-bit byteswapped */
            for (size_t i = 0; i + 1 < rom.size(); i += 2) {
                std::swap(rom[i], rom[i + 1]);
            }
        } else if (rom[0] == 0x40 && rom[1] == 0x12) { /* .n64: 32-bit little-endian */
            for (size_t i = 0; i + 3 < rom.size(); i += 4) {
                std::swap(rom[i], rom[i + 3]);
                std::swap(rom[i + 1], rom[i + 2]);
            }
        } else {
            std::snprintf(err, err_size, "that file isn't an N64 ROM");
            return 1;
        }
    }

    /* Torch's diagnostics (e.g. an unknown SHA-1) go to <data>/torch.log */
    auto previousLogger = spdlog::default_logger();
    try {
        spdlog::drop("torch");
        auto log = spdlog::basic_logger_mt("torch", (data / "torch.log").string(), true);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(log);
    } catch (const std::exception&) {
    }

    const fs::path o2r = data / "sf64.o2r";
    const fs::path temp = data / "sf64.o2r.importing";
    fs::remove_all(temp, ec);
    std::string outPath;
    bool ok = false;
    std::string why;
    try {
        /* Torch writes <dest>/sf64.o2r (config.yml's output.binary): a scratch
         * folder, so a failed run never leaves a partial archive */
        fs::create_directories(temp, ec);
        auto* c = new Companion(std::move(rom), ArchiveType::O2R, false, recipe.string(), temp.string());
        Companion::Instance = c;
        c->Init(ExportType::Binary);
        outPath = c->GetOutputPath();
        Companion::Instance = nullptr;
        delete c;
        ok = !outPath.empty() && fs::exists(outPath, ec);
        if (outPath.empty()) {
            why = "this isn't Star Fox 64 (USA 1.1)";
        } else if (!ok) {
            why = "not enough space to import the ROM";
        }
    } catch (const std::exception& e) {
        Companion::Instance = nullptr;
        why = std::string("extracting the game data failed: ") + e.what();
    }
    spdlog::drop("torch");
    spdlog::set_default_logger(previousLogger);
    if (ok) {
        fs::rename(outPath, o2r, ec);
        if (ec) {
            fs::remove(o2r, ec);
            fs::rename(outPath, o2r, ec);
        }
        ok = !ec;
        if (!ok) {
            why = "couldn't store the game data";
        }
    }
    fs::remove_all(temp, ec);
    if (!ok) {
        std::snprintf(err, err_size, "%s", why.c_str());
        return 1;
    }
    {
        std::ofstream stamp(o2r.string() + ".recipe", std::ios::trunc);
        stamp << SF64_ASSET_RECIPE << "\n";
    }
    if (progress != nullptr) {
        progress(1.0f, user);
    }
    return 0;
}

/* Online, on the session host: its save (the guests play on it, never write theirs). */
extern "C" N64_GAME_EXPORT uint32_t n64_game_session_blob(const n64_host* host, void* out, uint32_t cap) {
    Reentry_SavePristine();
    if (host == nullptr || sRunning || !SetDataHome(host)) {
        return 0;
    }
    uint32_t n = SfNet_SessionBlob(out, cap);
    if (host->log != nullptr) {
        host->log("sf64: session blob %u bytes", n);
    }
    return n;
}

extern "C" N64_GAME_EXPORT int n64_game_start(const n64_host* host, const n64_launch* launch, char* err,
                                              size_t err_size) {
    if (sRunning) {
        std::snprintf(err, err_size, "already running");
        return 1;
    }
    if (host == nullptr || launch == nullptr || host->abi_version != N64_GAME_ABI_VERSION ||
        launch->abi_version != N64_GAME_ABI_VERSION) {
        std::snprintf(err, err_size, "i64 and this game module disagree on the interface version");
        return 1;
    }
    const bool online = launch->mode == N64_LAUNCH_ONLINE;
    if (online && (host->net_send == nullptr || host->net_recv == nullptr || launch->local_player > 1)) {
        std::snprintf(err, err_size, "online needs i64's game channel and 2 players");
        return 1;
    }
    /* Game data: sf64.o2r is imported from the player's ROM (above),
     * starship.o2r comes with the module. */
    if (host->data_dir != nullptr && host->data_dir[0] != '\0') {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path data(host->data_dir);
        const fs::path o2r = data / "sf64.o2r";
        if (!fs::exists(o2r, ec)) {
            std::snprintf(err, err_size, "import your Star Fox 64 ROM first");
            return 1;
        }
        std::ifstream stamp(o2r.string() + ".recipe");
        std::string have;
        if (stamp && std::getline(stamp, have) && have != SF64_ASSET_RECIPE) {
            std::snprintf(err, err_size, "the game data is from another version: import the ROM again");
            return 1;
        }
        if (host->bundle_dir != nullptr && host->bundle_dir[0] != '\0') {
            const fs::path mine = fs::path(host->bundle_dir) / "starship.o2r";
            const fs::path used = data / "starship.o2r";
            if (fs::exists(mine, ec) && fs::path(host->bundle_dir) != data &&
                (!fs::exists(used, ec) || fs::file_size(mine, ec) != fs::file_size(used, ec) ||
                 fs::last_write_time(mine, ec) > fs::last_write_time(used, ec))) {
                fs::copy_file(mine, used, fs::copy_options::overwrite_existing, ec);
            }
        }
        if (!fs::exists(data / "starship.o2r", ec)) {
            std::snprintf(err, err_size, "starship.o2r is missing from the module");
            return 1;
        }
    }
    /* First start: what the module has done so far is static initialisation,
     * the pristine state to come back to after a run. */
    Reentry_SavePristine();

    sHost = host;
    gLusHostPaced = (launch->flags & N64_LAUNCH_HOST_PACED) != 0; /* libultraship: i64 paces the frames */
    sPreviewFrames = 0;
    gLusFrameCaptureCallback = host->offer_title_art;
    SetDataHome(host);
    /* netplay settings come from the environment: none from an earlier run */
    static const char* const kNetVars[] = { "SF64_NET", "SF64_NET_HOST", "SF64_NET_PEER", "SF64_NET_BIND",
                                            "SF64_NET_SERVER", "SF64_NET_ROOM", "SF64_NET_PASSWORD" };
    for (const char* v : kNetVars) {
        unsetenv(v);
    }
    sOnline = online;
    sPeerLeft = false;
    sNetHost = online ? host : nullptr;
    sPlayer = (int) launch->local_player;
    sSeed = launch->seed;
    sRun = launch->run;
    sBlob.clear();
    if (online && launch->session_blob_size > 0) {
        sBlob.assign((const uint8_t*) launch->session_blob, (const uint8_t*) launch->session_blob + launch->session_blob_size);
    } else if (online && sPlayer == 0) {
        /* i64 doesn't deliver session blobs yet: the host sends its own over the
         * game channel before the game starts (frames in the exchange phase) */
        uint8_t buf[2048];
        uint32_t n = SfNet_SessionBlob(buf, sizeof(buf));
        sBlob.assign(buf, buf + n);
    }
    if (sRendererWindow != host->window) {
        sRenderer = nullptr; /* a different window: LUS creates a new renderer */
        sRendererWindow = host->window;
    }
    LUS_SetExternalWindow(host->window, &sRenderer);
    LUS_SetExternalPacing((launch->flags & N64_LAUNCH_HOST_PACED) != 0);
    GameEngine_SetOnePicturePerHostFrame((launch->flags & N64_LAUNCH_HOST_PACED) != 0 &&
                                         !getenv("SF64_ALL_PICTURES_PER_TICK"));   // test aid: the old way
    gPortHostAudioWrite = host->audio_write;
    gPortHostAudioBuffered = host->audio_buffered;
    LUS_SetExternalAudio(host->audio_write != nullptr);   /* no audio device of our own */

    /* the rollback snapshot ring (16 slots, up to ~5 MB each) in i64's game memory arena
     * (i64 docs/game-memory-arena.md); without one, malloc as before */
    if (host->arena_reserve != nullptr && host->arena_size != 0) {
        const size_t want = ((Snapshot_MaxBytes() * 16) + 0x3FFF) & ~(size_t) 0x3FFF;
        const uint64_t a = want ? host->arena_reserve(want, 0) : 0;
        if (a != 0 && mprotect((void*) (uintptr_t) a, want, PROT_READ | PROT_WRITE) == 0) {
            Snapshot_SetStorage((void*) (uintptr_t) a, want);
            if (host->log) host->log("sf64: snapshots: %zu MB block from the game memory arena", want >> 20);
        } else if (host->log && want) {
            host->log("sf64: snapshots: no room in the game memory arena for %zu MB; using malloc", want >> 20);
        }
    }
    sRunning = true;
    sExchangeFrames = 0;
    if (online && launch->session_blob_size == 0) {
        sPhase = kPhaseExchange;   /* BeginGame once both sides have the host's blob */
        return 0;
    }
    if (!BeginGame()) {
        std::snprintf(err, err_size, "the host's game data didn't arrive or is from another version");
        sRunning = false;
        return 1;
    }
    return 0;
}

#include <chrono>
static uint64_t StatsNowMs() {
    return (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

extern "C" N64_GAME_EXPORT n64_frame_result n64_game_frame(void) {
    if (!sRunning) {
        return N64_GAME_FRAME_EXIT;
    }
    ++sPreviewFrames;
    if (PreviewDue(sPreviewFrames) && gLusFrameCaptureCallback) {
        gLusFrameCaptureRequest = 1;
    }
    PollEvents();
    if (sOnline && sPeerLeft) {
        return N64_GAME_FRAME_EXIT; /* the other player left: back to i64 */
    }
    if (sPhase == kPhaseExchange) {
        return ExchangeFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
    }
    if (sOnline && SfNet_EndReason()[0]) {
        return N64_GAME_FRAME_EXIT;   /* a desync or the friend gone: i64 shows why (n64_game_exit_message) */
    }
    // speed check: logic ticks per host frame (Star Fox's logic is 30 Hz in most scenes)
    static uint64_t sStatsAt = 0;
    static unsigned sHostFrames = 0, sLogicTicks = 0;
    sHostFrames++;
    if (StatsNowMs() >= sStatsAt) {
        if (sStatsAt && sHost && sHost->log) {
            sHost->log("sf64: %u host frames, %u logic ticks in the last 5 s", sHostFrames, sLogicTicks);
        }
        sHostFrames = sLogicTicks = 0;
        sStatsAt = StatsNowMs() + 5000;
    }
    // a host frame belonging to the last logic tick (gVIsPerFrame refreshes each): its next picture
    if (GameEngine_HostFrameWithoutLogic()) {
        return N64_GAME_FRAME_CONTINUE;
    }
    sLogicTicks++;
    return Port_RunFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
}

/* i64 keeps the game while its players are in i64's menu. Online the peer
 * waits for us without a limit (SfNet away), as when the app is backgrounded. */
/* the game's pause state (src/engine/fox_play.c), for opening its own pause menu */
extern "C" {
extern int gGameState;      /* GameState: GSTATE_PLAY = 7 */
extern int32_t gPlayState;  /* PLAY_UPDATE = 2, PLAY_PAUSE = 100 */
extern uint8_t gPauseEnabled;
extern int gPortInjectStart; /* sys_joybus.c: one Start press at the next input update */
}
static bool sWaitPauseMenu = false;

extern "C" N64_GAME_EXPORT void n64_game_set_paused(int paused) {
    if (!sRunning) {
        return;
    }
    if (sOnline) {
        SfNet_SetAway(paused == 1 ? 1 : 0);
        return;
    }
    /* offline, back to i64 mid-mission: open the game's pause menu first */
    sWaitPauseMenu = false;
    if (paused == 1 && gGameState == 7 && gPlayState == 2 && gPauseEnabled) {
        gPortInjectStart = 1;
        sWaitPauseMenu = true;
    }
}

extern "C" N64_GAME_EXPORT void n64_game_exit_message(char* out, size_t cap) {
    std::snprintf(out, cap, "%s", sOnline ? SfNet_EndReason() : "");
}

/* offline: ready once the pause menu is open (or the game left play meanwhile) */
extern "C" N64_GAME_EXPORT int n64_game_pause_ready(void) {
    if (!sRunning || sOnline || !sWaitPauseMenu) {
        return 1;
    }
    if (gPlayState == 100 || gGameState != 7 || gPlayState != 2) {
        sWaitPauseMenu = false;
        if (sHost && sHost->log) {
            sHost->log("sf64: %s", gPlayState == 100 ? "the game's pause menu is open" : "left play before pausing");
        }
        return 1;
    }
    return 0;
}

extern "C" N64_GAME_EXPORT void n64_game_stop(void) {
    gLusFrameCaptureCallback = nullptr;
    gLusFrameCaptureRequest = 0;
    if (!sRunning) {
        return;
    }
    if (sPhase == kPhaseGame) {
        Port_RunEnd(1);
    }
    sPhase = kPhaseNone;
    std::vector<uint8_t>().swap(sBlob);
    gPortHostAudioWrite = nullptr;
    gPortHostAudioBuffered = nullptr;
    sOnline = false;
    sNetHost = nullptr;
    LUS_SetExternalWindow(nullptr, nullptr);
    LUS_SetExternalPacing(false);
    GameEngine_SetOnePicturePerHostFrame(false);
    LUS_SetExternalAudio(false);
    Snapshot_SetStorage(nullptr, 0); /* i64 wipes the arena after this */
    Reentry_RestorePristine();
    sRunning = false;
    sHost = nullptr;
}

#endif /* STARSHIP_I64_MODULE */
