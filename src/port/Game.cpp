#include <libultraship.h>

#include <Fast3D/interpreter.h>
#include "Engine.h"

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

#ifdef _WIN32
int SDL_main(int argc, char **argv) {
#else
#if defined(__cplusplus) && defined(PLATFORM_IOS)
extern "C"
#endif
int main(int argc, char *argv[]) {
#endif
    GameEngine::Create();
    Main_SetVIMode();
    Lib_FillScreen(1);
    Main_Initialize();
    Main_ThreadEntry(NULL);
    SDL_AddEventWatch(AppEventWatch, nullptr);
    while (WindowIsRunning()) {
        SfNet_SetAway(sInBackground);
        if (sInBackground) {
            SDL_PumpEvents(); // notices the return to the foreground
            SDL_Delay(10);
            continue;
        }
        push_frame();
    }
    GameEngine::Instance->Destroy();
    return 0;
}