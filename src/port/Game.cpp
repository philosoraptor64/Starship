#include "port/interpolation/FrameInterpolation.h"
#include <libultraship.h>

#include <Fast3D/interpreter.h>
#include "Engine.h"
#include "resource/type/audio/Sample.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

extern "C" {
#include <sf64mesg.h>
    void Main_SetVIMode(void);
    void Main_Initialize(void);
    void Main_ThreadEntry(void* arg);
    void Lib_FillScreen(u8 setFill);
    void Graphics_ThreadUpdate();
    void AudioThread_CreateTask();
#include "port/SfNet.h"
}

// @port: iOS app in the background: no game ticks and no rendering (Metal work in the background made the game
// come back at ~1 fps in our other ports); online, the other player waits for us (SfNet away)
static volatile bool sInBackground = false;

static int AppEventWatch(void* user, SDL_Event* event) {
    (void) user;
    if (event->type == SDL_APP_WILLENTERBACKGROUND) {
        sInBackground = true;
    } else if (event->type == SDL_APP_DIDENTERFOREGROUND) {
        sInBackground = false;
    }
    return 1;
}

extern "C" int Port_IsInBackground(void) {
    return sInBackground;
}

extern "C"
void Graphics_PushFrame(Gfx* data) {
    GameEngine::ProcessGfxCommands(data);
}

extern "C" void Timer_Update();

void push_frame() {
    Graphics_ThreadUpdate();
    GameEngine::StartAudioFrame();
    GameEngine::Instance->StartFrame();
    Timer_Update();
    // thread5_iteration();
    GameEngine::EndAudioFrame();
}

/* One run in three steps: the stand-alone loop below, or the i64 module
 * (src/port/i64_module.cpp) calling them from the host's frame loop. */
static bool sStopRequested = false;
extern "C" void Port_RequestStop(void) {
    sStopRequested = true;
}

extern "C" void Port_RunBegin(void) {
    sStopRequested = false;
    sInBackground = false;
    GameEngine::Create();
    Main_SetVIMode();
    Lib_FillScreen(1);
    Main_Initialize();
    Main_ThreadEntry(NULL);
    SDL_AddEventWatch(AppEventWatch, nullptr);
}

extern "C" int Port_RunFrame(void) {
    if (!WindowIsRunning() || sStopRequested) {
        return 0;
    }
    SfNet_SetAway(sInBackground);
    if (sInBackground) {
        SDL_PumpEvents(); // notices the return to the foreground
        return 1;
    }
    push_frame();
    return 1;
}

extern "C" void Port_RunEnd(int fullShutdown) {
    // Inside a host (i64) the process outlives the run: the watch must go, or it
    // piles up and keeps calling into this game's code while another game runs.
    SDL_DelEventWatch(AppEventWatch, nullptr);
    SfNet_Shutdown();
    GameEngine::Instance->Destroy();
    if (fullShutdown) {
        // the engine holds the only strong reference to libultraship's context (window,
        // renderer, resource threads, audio, config): deleting it shuts all of that down
        FrameInterpolation_ReleaseForReentry();
        SF64::gAudioSampleFreeData = true;   // the audio thread is stopped: samples go with their resources
        delete GameEngine::Instance;
        GameEngine::Instance = nullptr;
        SF64::gAudioSampleFreeData = false;
        if (Ship::Context::GetInstance() != nullptr) {
            SPDLOG_WARN("libultraship's context outlived the engine (another owner)");
        }
        spdlog::shutdown();
        // ...and a plain default logger again (what a fresh process has) for
        // the lines the next start writes before libultraship sets up its own
        spdlog::set_default_logger(spdlog::stderr_color_mt("sf64"));
    }
}

#if !defined(STARSHIP_I64_MODULE)
#ifdef _WIN32
int SDL_main(int argc, char **argv) {
#else
#if defined(__cplusplus) && defined(PLATFORM_IOS)
extern "C"
#endif
int main(int argc, char *argv[]) {
#endif
    Port_RunBegin();
    while (Port_RunFrame()) {
        if (sInBackground) {
            SDL_Delay(10);
        }
    }
    Port_RunEnd(0);
    return 0;
}
#endif
