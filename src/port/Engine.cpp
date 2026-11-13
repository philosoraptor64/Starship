#include "Engine.h"
#include "ui/ImguiUI.h"
#include "StringHelper.h"

#include "extractor/GameExtractor.h"
#include "libultraship/src/Context.h"
#include "libultraship/src/controller/controldevice/controller/mapping/ControllerDefaultMappings.h"
#include "resource/type/ResourceType.h"
#include "resource/importers/AnimFactory.h"
#include "resource/importers/ColPolyFactory.h"
#include "resource/importers/EnvSettingsFactory.h"
#include "resource/importers/GenericArrayFactory.h"
#include "resource/importers/HitboxFactory.h"
#include "resource/importers/LimbFactory.h"
#include "resource/importers/MessageFactory.h"
#include "resource/importers/MessageLookupFactory.h"
#include "resource/importers/ObjectInitFactory.h"
#include "resource/importers/ScriptCommandFactory.h"
#include "resource/importers/ScriptFactory.h"
#include "resource/importers/SkeletonFactory.h"
#include "resource/importers/Vec3fFactory.h"
#include "resource/importers/Vec3sFactory.h"

#include "resource/importers/audio/AudioTableFactory.h"
#include "resource/importers/audio/BookFactory.h"
#include "resource/importers/audio/DrumFactory.h"
#include "resource/importers/audio/EnvelopeFactory.h"
#include "resource/importers/audio/InstrumentFactory.h"
#include "resource/importers/audio/LoopFactory.h"
#include "resource/importers/audio/SampleFactory.h"
#include "resource/importers/audio/SoundFontFactory.h"

#include "port/interpolation/FrameInterpolation.h"
#include <Fast3D/Fast3dWindow.h>
#include <DisplayListFactory.h>
#include <TextureFactory.h>
#include <MatrixFactory.h>
#include <BlobFactory.h>
#include <VertexFactory.h>
#include "audio/GameAudio.h"
#include "port/patches/DisplayListPatch.h"
#include "port/mods/PortEnhancements.h"

#include <Fast3D/interpreter.h>
#include <filesystem>

#ifdef __SWITCH__
#include <port/switch/SwitchImpl.h>
#endif

namespace fs = std::filesystem;

extern "C" {
bool prevAltAssets = false;
bool gEnableGammaBoost = true;
#include <sf64thread.h>
#include <macros.h>
#include "sf64audio_provisional.h"
void AudioThread_CreateNextAudioBuffer(int16_t* samples, uint32_t num_samples);
}

std::vector<uint8_t*> MemoryPool;
GameEngine* GameEngine::Instance;

GameEngine::GameEngine() {
    // Initialize context properties early to recognize paths properly for non-portable builds
    this->context = Ship::Context::CreateUninitializedInstance("Starship", "ship", "starship.cfg.json");

#ifdef __SWITCH__
    Ship::Switch::Init(Ship::PreInitPhase);
    Ship::Switch::Init(Ship::PostInitPhase);
#endif

    std::vector<std::string> archiveFiles;
    const std::string main_path = Ship::Context::GetPathRelativeToAppDirectory("sf64.o2r");
    const std::string assets_path = Ship::Context::LocateFileAcrossAppDirs("starship.o2r");

#ifdef _WIN32
    AllocConsole();
#endif

    if (std::filesystem::exists(main_path)) {
        archiveFiles.push_back(main_path);
    } else {
        if (ShowYesNoBox("Starship - Asset Extraction", "Please provide a Starfox 64 ROM.\n\nSupported Versions:\nUS 1.0\nUS 1.1\n\nAssets will be extracted into an O2R file.") == IDYES) {
            if(!GenAssetFile()){
                ShowMessage("Error", "An error occured, no O2R file was generated.\n\nExiting...");
                exit(1);
            } else {
                archiveFiles.push_back(main_path);
            }

            if (ShowYesNoBox("Extraction Complete", "ROM Extracted. Extract another?\n\n Starship supports JP and EU ROMs for voice replacement.\n Voice replacement ROM assets can also be installed in:\n Settings->Language->Install JP/EU Audio") == IDYES) {
                if(!GenAssetFile()){
                    ShowMessage("Error", "An error occured, no O2R file was generated.");
                }
            }
        } else {
            exit(1);
        }
    }

    if (std::filesystem::exists(assets_path)) {
        archiveFiles.push_back(assets_path);
    }

    if (const std::string patches_path = Ship::Context::GetPathRelativeToAppDirectory("mods");
        !patches_path.empty()) {
        if (!std::filesystem::exists(patches_path)) {
            std::filesystem::create_directories(patches_path);
        }

        if (std::filesystem::is_directory(patches_path)) {
            for (const auto& p : std::filesystem::recursive_directory_iterator(patches_path)) {
                const auto ext = p.path().extension().string();
                if (StringHelper::IEquals(ext, ".otr") || StringHelper::IEquals(ext, ".o2r")) {
                    archiveFiles.push_back(p.path().generic_string());
                }

                if (StringHelper::IEquals(ext, ".zip")) {
                    SPDLOG_WARN("Zip files should be only used for development purposes, not for distribution");
                    archiveFiles.push_back(p.path().generic_string());
                }
            }
        }
    }

    this->context->InitConfiguration();    // without this line InitConsoleVariables fails at Config::Reload()
    this->context->InitConsoleVariables(); // without this line the controldeck constructor failes in
                                           // ShipDeviceIndexMappingManager::UpdateControllerNamesFromConfig()

    auto defaultMappings = std::make_shared<Ship::ControllerDefaultMappings>(
        // KeyboardKeyToButtonMappings - use built-in LUS defaults
        std::unordered_map<CONTROLLERBUTTONS_T, std::unordered_set<Ship::KbScancode>>(),
        // KeyboardKeyToAxisDirectionMappings - use built-in LUS defaults
        std::unordered_map<Ship::StickIndex, std::vector<std::pair<Ship::Direction, Ship::KbScancode>>>(),
        // SDLButtonToButtonMappings
        std::unordered_map<CONTROLLERBUTTONS_T, std::unordered_set<SDL_GameControllerButton>>{
            { BTN_A, { SDL_CONTROLLER_BUTTON_A } },
            { BTN_B, { SDL_CONTROLLER_BUTTON_X } },
            { BTN_START, { SDL_CONTROLLER_BUTTON_START } },
            // The right stick aims, so the C buttons also sit on the D-pad (user's choice, 2026-11-10).
            { BTN_CLEFT, { SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_DPAD_LEFT } },
            { BTN_CDOWN, { SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_DPAD_DOWN } },
            { BTN_CUP, { SDL_CONTROLLER_BUTTON_DPAD_UP } },
            { BTN_CRIGHT, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT } },
            { BTN_DUP, { SDL_CONTROLLER_BUTTON_DPAD_UP } },
            { BTN_DDOWN, { SDL_CONTROLLER_BUTTON_DPAD_DOWN } },
            { BTN_DLEFT, { SDL_CONTROLLER_BUTTON_DPAD_LEFT } },
            { BTN_DRIGHT, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT } },
            { BTN_R, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER } },
            { BTN_Z, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER } }
        },
        // SDLButtonToAxisDirectionMappings - use built-in LUS defaults
        std::unordered_map<Ship::StickIndex, std::vector<std::pair<Ship::Direction, SDL_GameControllerButton>>>(),
        // SDLAxisDirectionToButtonMappings
        std::unordered_map<CONTROLLERBUTTONS_T, std::vector<std::pair<SDL_GameControllerAxis, int32_t>>>{
            { BTN_CLEFT, { { SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1 } } },
            { BTN_CDOWN, { { SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 } } },
            { BTN_CUP, { { SDL_CONTROLLER_AXIS_RIGHTY, -1 } } },
            { BTN_CRIGHT, { { SDL_CONTROLLER_AXIS_RIGHTX, 1 } } }
        },
        // SDLAxisDirectionToAxisDirectionMappings - use built-in LUS defaults
        std::unordered_map<Ship::StickIndex, std::vector<std::pair<Ship::Direction, std::pair<SDL_GameControllerAxis, int32_t>>>>()
    );
    auto controlDeck = std::make_shared<LUS::ControlDeck>(std::vector<CONTROLLERBUTTONS_T>(), defaultMappings);

    this->context->InitResourceManager(archiveFiles, {}, 3); // without this line InitWindow fails in Gui::Init()
    this->context->InitConsole(); // without this line the GuiWindow constructor fails in ConsoleWindow::InitElement()

    auto window = std::make_shared<Fast::Fast3dWindow>(std::vector<std::shared_ptr<Ship::GuiWindow>>({}));

    auto audioChannelsSetting = Ship::Context::GetInstance()->GetConfig()->GetCurrentAudioChannelsSetting();
    this->context->Init(archiveFiles, {}, 3, { 32000, 1024, 1680, audioChannelsSetting }, window, controlDeck);

#ifndef __SWITCH__
    Ship::Context::GetInstance()->GetLogger()->set_level(
        (spdlog::level::level_enum) CVarGetInteger("gDeveloperTools.LogLevel", 1));
    Ship::Context::GetInstance()->GetLogger()->set_pattern("[%H:%M:%S.%e] [%s:%#] [%l] %v");
#endif

    auto loader = context->GetResourceManager()->GetResourceLoader();
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryAnimV0>(), RESOURCE_FORMAT_BINARY,
                                    "Animation", static_cast<uint32_t>(SF64::ResourceType::AnimData), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinarySkeletonV0>(), RESOURCE_FORMAT_BINARY,
                                    "Skeleton", static_cast<uint32_t>(SF64::ResourceType::Skeleton), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryLimbV0>(), RESOURCE_FORMAT_BINARY,
                                    "Limb", static_cast<uint32_t>(SF64::ResourceType::Limb), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryMessageV0>(), RESOURCE_FORMAT_BINARY,
                                    "Message", static_cast<uint32_t>(SF64::ResourceType::Message), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryMessageLookupV0>(),
                                    RESOURCE_FORMAT_BINARY, "MessageTable",
                                    static_cast<uint32_t>(SF64::ResourceType::MessageTable), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryEnvSettingsV0>(),
                                    RESOURCE_FORMAT_BINARY, "EnvSettings",
                                    static_cast<uint32_t>(SF64::ResourceType::Environment), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryObjectInitV0>(), RESOURCE_FORMAT_BINARY,
                                    "ObjectInit", static_cast<uint32_t>(SF64::ResourceType::ObjectInit), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryHitboxV0>(), RESOURCE_FORMAT_BINARY,
                                    "Hitbox", static_cast<uint32_t>(SF64::ResourceType::Hitbox), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryScriptV0>(), RESOURCE_FORMAT_BINARY,
                                    "Script", static_cast<uint32_t>(SF64::ResourceType::Script), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryScriptCMDV0>(), RESOURCE_FORMAT_BINARY,
                                    "ScriptCMD", static_cast<uint32_t>(SF64::ResourceType::ScriptCmd), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryColPolyV0>(), RESOURCE_FORMAT_BINARY,
                                    "ColPoly", static_cast<uint32_t>(SF64::ResourceType::ColPoly), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryVec3fV0>(), RESOURCE_FORMAT_BINARY,
                                    "Vec3f", static_cast<uint32_t>(SF64::ResourceType::Vec3f), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryVec3sV0>(), RESOURCE_FORMAT_BINARY,
                                    "Vec3s", static_cast<uint32_t>(SF64::ResourceType::Vec3s), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryGenericArrayV0>(),
                                    RESOURCE_FORMAT_BINARY, "GenericArray",
                                    static_cast<uint32_t>(SF64::ResourceType::GenericArray), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV0>(), RESOURCE_FORMAT_BINARY,
                                    "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(), RESOURCE_FORMAT_BINARY,
                                    "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 1);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryVertexV0>(), RESOURCE_FORMAT_BINARY,
                                    "Vertex", static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryXMLVertexV0>(), RESOURCE_FORMAT_XML, "Vertex",
                                    static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryDisplayListV0>(),
                                    RESOURCE_FORMAT_BINARY, "DisplayList",
                                    static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryXMLDisplayListV0>(), RESOURCE_FORMAT_XML,
                                    "DisplayList", static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryMatrixV0>(), RESOURCE_FORMAT_BINARY,
                                    "Matrix", static_cast<uint32_t>(Fast::ResourceType::Matrix), 0);

    loader->RegisterResourceFactory(std::make_shared<Ship::ResourceFactoryBinaryBlobV0>(), RESOURCE_FORMAT_BINARY,
                                    "Blob", static_cast<uint32_t>(Ship::ResourceType::Blob), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryAudioTableV0>(), RESOURCE_FORMAT_BINARY,
                                    "AudioTable", static_cast<uint32_t>(SF64::ResourceType::AudioTable), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryAdpcmBookV0>(), RESOURCE_FORMAT_BINARY,
                                    "AdpcmBook", static_cast<uint32_t>(SF64::ResourceType::AdpcmBook), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryDrumV0>(), RESOURCE_FORMAT_BINARY,
                                    "Drum", static_cast<uint32_t>(SF64::ResourceType::Drum), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryEnvelopeV0>(), RESOURCE_FORMAT_BINARY,
                                    "Envelope", static_cast<uint32_t>(SF64::ResourceType::Envelope), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryInstrumentV0>(), RESOURCE_FORMAT_BINARY,
                                    "Instrument", static_cast<uint32_t>(SF64::ResourceType::Instrument), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryAdpcmLoopV0>(), RESOURCE_FORMAT_BINARY,
                                    "AdpcmLoop", static_cast<uint32_t>(SF64::ResourceType::AdpcmLoop), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinarySampleV1>(), RESOURCE_FORMAT_BINARY,
                                    "Sample", static_cast<uint32_t>(SF64::ResourceType::Sample), 1);
    
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryXMLSampleV0>(), RESOURCE_FORMAT_XML,
                                    "Sample", static_cast<uint32_t>(SF64::ResourceType::Sample), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinarySoundFontV0>(), RESOURCE_FORMAT_BINARY,
                                    "SoundFont", static_cast<uint32_t>(SF64::ResourceType::SoundFont), 0);

    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryXMLSoundFontV0>(), RESOURCE_FORMAT_XML,
                                    "SoundFont", static_cast<uint32_t>(SF64::ResourceType::SoundFont), 0);

    prevAltAssets = CVarGetInteger("gEnhancements.Mods.AlternateAssets", 0);
    gEnableGammaBoost = CVarGetInteger("gGraphics.GammaMode", 0) == 0;
    context->GetResourceManager()->SetAltAssetsEnabled(prevAltAssets);
}

bool GameEngine::GenAssetFile(bool exitOnFail) {
    auto extractor = new GameExtractor();

    if (!extractor->SelectGameFromUI()) {
        ShowMessage("Error", "No ROM selected.\n\nExiting...");
        if (exitOnFail) {
            exit(1);
        } else {
            return false;
        }
    }

    auto game = extractor->ValidateChecksum();
    if (!game.has_value()) {
        ShowMessage("Unsupported ROM", "The provided ROM is not supported.\n\nCheck the readme for a list of supported versions.");
        if (exitOnFail) {
            exit(1);
        } else {
            return false;
        }
    }

    ShowMessage(("Starship - Extraction - Found " + game.value()).c_str(), "The extraction process will now begin.\n\nThis may take a few minutes.", SDL_MESSAGEBOX_INFORMATION);

    return extractor->GenerateOTR();
}

void GameEngine::Create() {
    const auto instance = Instance = new GameEngine();
    instance->AudioInit();
    DisplayListPatch::Run();
    GameUI::SetupGuiElements();
#if defined(__SWITCH__) || defined(__WIIU__)
    CVarRegisterInteger("gControlNav", 1); // always enable controller nav on switch/wii u
    osSetTime(0);
#endif
    PortEnhancements_Init();
}

void GameEngine::Destroy() {
    PortEnhancements_Exit();
    AudioExit();
    for (auto ptr : MemoryPool) {
        free(ptr);
    }
    MemoryPool.clear();
#ifdef __SWITCH__
    Ship::Switch::Exit();
#endif
}

void GameEngine::StartFrame() const {
    using Ship::KbScancode;
    const int32_t dwScancode = this->context->GetWindow()->GetLastScancode();
    this->context->GetWindow()->SetLastScancode(-1);

    switch (dwScancode) {
        case KbScancode::LUS_KB_TAB: {
            // Toggle HD Assets
            CVarSetInteger("gEnhancements.Mods.AlternateAssets", !CVarGetInteger("gEnhancements.Mods.AlternateAssets", 0));
            break;
        }
        case KbScancode::LUS_KB_F4: {
            gNextGameState = GSTATE_BOOT;
            break;
        }
        default:
            break;
    }
}

#if 0
// Values for 44100 hz
#define SAMPLES_HIGH 752
#define SAMPLES_LOW 720
#else
// Values for 32000 hz
#define SAMPLES_HIGH 560
#define SAMPLES_LOW 528

#endif

#define MAX_NUM_AUDIO_CHANNELS 6

extern "C" u16 audBuffer = 0;
#include <sf64audio_provisional.h>

extern "C" volatile s32 gAudioTaskCountQ;
int frames = 0;
extern "C" int countermin = 0;

extern "C" unsigned short samples_high = SAMPLES_HIGH;
extern "C" unsigned short samples_low = SAMPLES_LOW;

/* Inside a host app (the i64 launcher): the game's audio goes to the host,
 * which mixes and plays it; set by src/port/i64_module.cpp for a run. */
extern "C" {
void (*gPortHostAudioWrite)(const int16_t* stereo, uint32_t frames, uint32_t rate) = nullptr;
uint32_t (*gPortHostAudioBuffered)(uint32_t rate) = nullptr;
}
static constexpr uint32_t kHostAudioRate = 32000; /* the SAMPLES_HIGH/LOW values above are for 32 kHz */

static int PortAudioBuffered() {
    if (gPortHostAudioWrite != nullptr && gPortHostAudioBuffered != nullptr) {
        return (int) gPortHostAudioBuffered(kHostAudioRate);
    }
    return AudioPlayerBuffered();
}

static void PortAudioPlay(s16* samples, uint32_t frames, int channels) {
    if (gPortHostAudioWrite != nullptr) {
        if (channels == 2) {
            gPortHostAudioWrite(samples, frames, kHostAudioRate);
        } else {   /* the host takes stereo: front left/right */
            static s16 stereo[SAMPLES_HIGH * 5 * 2];
            uint32_t n = frames < SAMPLES_HIGH * 5 ? frames : SAMPLES_HIGH * 5;
            for (uint32_t i = 0; i < n; i++) {
                stereo[2 * i] = samples[i * channels];
                stereo[2 * i + 1] = samples[i * channels + (channels > 1 ? 1 : 0)];
            }
            gPortHostAudioWrite(stereo, n, kHostAudioRate);
        }
        return;
    }
    AudioPlayerPlayFrame((u8*) samples, frames * channels * sizeof(int16_t));
}

void GameEngine::HandleAudioThread() {
#ifdef PIPE_DEBUG
    std::ofstream outfile("audio.bin", std::ios::binary | std::ios::app);
#endif
    while (audio.running) {
        {
            std::unique_lock<std::mutex> Lock(audio.mutex);
            while (!audio.processing && audio.running) {
                audio.cv_to_thread.wait(Lock);
            }
            if (!audio.running) {
                break;
            }
        }

        // gVIsPerFrame = 2;

#define AUDIO_FRAMES_PER_UPDATE (gVIsPerFrame > 0 ? gVIsPerFrame : 1)
#define MAX_AUDIO_FRAMES_PER_UPDATE 5 // Compile-time constant with max value of gVIsPerFrame

        std::unique_lock<std::mutex> Lock(audio.mutex);
        int samples_left = PortAudioBuffered();
        u32 num_audio_samples = samples_left < AudioPlayerGetDesiredBuffered() ? (((samples_high))) : (((samples_low)));

        frames++;

        if (frames > 60) {
            countermin++;
        }

        const int32_t num_audio_channels = GetNumAudioChannels();

        s16 audio_buffer[SAMPLES_HIGH * MAX_NUM_AUDIO_CHANNELS * MAX_AUDIO_FRAMES_PER_UPDATE] = { 0 };
        for (int i = 0; i < AUDIO_FRAMES_PER_UPDATE; i++) {
            AudioThread_CreateNextAudioBuffer(audio_buffer + i * (num_audio_samples * num_audio_channels),
                                              num_audio_samples);
        }
#ifdef PIPE_DEBUG
        if (outfile.is_open()) {
            outfile.write(reinterpret_cast<char*>(audio_buffer),
                          num_audio_samples * (sizeof(int16_t) * num_audio_channels * AUDIO_FRAMES_PER_UPDATE));
        }
#endif
        PortAudioPlay(audio_buffer, num_audio_samples * AUDIO_FRAMES_PER_UPDATE, num_audio_channels);
        
        audio.processing = false;
        audio.cv_from_thread.notify_one();
    }
#ifdef PIPE_DEBUG
    outfile.close();
#endif
}

void GameEngine::StartAudioFrame() {
    {
        std::unique_lock<std::mutex> Lock(audio.mutex);
        audio.processing = true;
    }
    audio.cv_to_thread.notify_one();
}

void GameEngine::EndAudioFrame() {
    {
        std::unique_lock<std::mutex> Lock(audio.mutex);
        while (audio.processing) {
            audio.cv_from_thread.wait(Lock);
        }
    }
}

void GameEngine::AudioInit() {
    if (!audio.running) {
        audio.running = true;
        audio.thread = std::thread(HandleAudioThread);
    }
}

void GameEngine::AudioExit() {
    {
        std::unique_lock lock(audio.mutex);
        audio.running = false;
    }
    audio.cv_to_thread.notify_all();
    // Wait until the audio thread quit
    audio.thread.join();
}

// i64 (host-paced: the host calls one frame per display refresh): each host frame presents exactly one
// picture. The game's logic runs once per gVIsPerFrame refreshes (30 Hz in most of Star Fox) and the
// frame interpolation makes gVIsPerFrame pictures per logic tick; drawing them all in one host frame
// made that frame wait for two refreshes (30 host frames/s on the iPhone) and, where the device kept
// up, ran the logic every host frame (double speed on the Mac). Now the first picture is drawn with
// the logic tick and the others in the following host frames (GameEngine_HostFrameWithoutLogic).
static bool sOnePicturePerHostFrame = false;
static Gfx* sPendingCommands = nullptr;
static std::vector<std::unordered_map<Mtx*, MtxF>> sPendingPictures;
static size_t sPendingNext = 0;
static int sHostFramesLeft = 0;   // host frames still belonging to the last logic tick

extern "C" void GameEngine_SetOnePicturePerHostFrame(bool on) {
    sOnePicturePerHostFrame = on;
    sPendingCommands = nullptr;
    sPendingPictures.clear();
    sPendingNext = 0;
    sHostFramesLeft = 0;
}

// a host frame that belongs to the last logic tick: its next interpolated picture, if any. Returns 0 when
// it's time for the next logic tick (call push_frame).
extern "C" int GameEngine_HostFrameWithoutLogic(void) {
    if (!sOnePicturePerHostFrame || sHostFramesLeft <= 0) {
        return 0;
    }
    sHostFramesLeft--;
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
    if (wnd != nullptr && sPendingCommands != nullptr && sPendingNext < sPendingPictures.size()) {
        auto interpreter = wnd->GetInterpreterWeak().lock().get();
        wnd->HandleEvents();
        interpreter->mInterpolationIndex = (int) sPendingNext;
        wnd->DrawAndRunGraphicsCommands(sPendingCommands, sPendingPictures[sPendingNext]);
        sPendingNext++;
    }
    return 1;
}

void GameEngine::RunCommands(Gfx* Commands, const std::vector<std::unordered_map<Mtx*, MtxF>>& mtx_replacements) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    auto interpreter = wnd->GetInterpreterWeak().lock().get();

    // Process window events for resize, mouse, keyboard events
    wnd->HandleEvents();

    interpreter->mInterpolationIndex = 0;

    if (sOnePicturePerHostFrame) {
        // this host frame: the first picture; the rest (and empty host frames up to gVIsPerFrame) follow
        if (!mtx_replacements.empty()) {
            wnd->DrawAndRunGraphicsCommands(Commands, mtx_replacements[0]);
        }
        sPendingCommands = Commands;
        sPendingPictures.assign(mtx_replacements.begin() + (mtx_replacements.empty() ? 0 : 1), mtx_replacements.end());
        sPendingNext = 0;
        sHostFramesLeft = std::max<int>((int) gVIsPerFrame, 1) - 1;
    } else {
        for (const auto& m : mtx_replacements) {
            wnd->DrawAndRunGraphicsCommands(Commands, m);
            interpreter->mInterpolationIndex++;
        }
    }

    bool curAltAssets = CVarGetInteger("gEnhancements.Mods.AlternateAssets", 0);
    if (prevAltAssets != curAltAssets) {
        prevAltAssets = curAltAssets;
        Ship::Context::GetInstance()->GetResourceManager()->SetAltAssetsEnabled(curAltAssets);
        gfx_texture_cache_clear();
    }
}

void GameEngine::ProcessGfxCommands(Gfx* commands) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    if(gEnableGammaBoost) {
        wnd->EnableSRGBMode();
    }
    wnd->SetRendererUCode(UcodeHandlers::ucode_f3dex);

    std::vector<std::unordered_map<Mtx*, MtxF>> mtx_replacements;
    int target_fps = GameEngine::Instance->GetInterpolationFPS();
    static int last_fps;
    static int last_update_rate;
    static int time;
    int fps = target_fps;
    int original_fps = 60 / gVIsPerFrame;

    if (target_fps == 20 || original_fps > target_fps) {
        fps = original_fps;
    }

    if (last_fps != fps || last_update_rate != gVIsPerFrame) {
        time = 0;
    }

    // time_base = fps * original_fps (one second)
    int next_original_frame = fps;

    while (time + original_fps <= next_original_frame) {
        time += original_fps;
        if (time != next_original_frame) {
            mtx_replacements.push_back(FrameInterpolation_Interpolate((float) time / next_original_frame));
        } else {
            mtx_replacements.emplace_back();
        }
    }

    time -= fps;

    if (wnd != nullptr) {
        wnd->SetTargetFps(fps);
        wnd->SetMaximumFrameLatency(CVarGetInteger("gRenderParallelization", 1) ? 2 : 1);
    }

    // When the gfx debugger is active, only run with the final mtx
    if (GfxDebuggerIsDebugging()) {
        mtx_replacements.clear();
        mtx_replacements.emplace_back();
    }

    RunCommands(commands, mtx_replacements);

    last_fps = fps;
    last_update_rate = gVIsPerFrame;
}

uint32_t GameEngine::GetInterpolationFPS() {
    if (CVarGetInteger("gMatchRefreshRate", 0)) {
        return Ship::Context::GetInstance()->GetWindow()->GetCurrentRefreshRate();

    } else if (CVarGetInteger("gVsyncEnabled", 1) ||
               !Ship::Context::GetInstance()->GetWindow()->CanDisableVerticalSync()) {
        return std::min<uint32_t>(Ship::Context::GetInstance()->GetWindow()->GetCurrentRefreshRate(),
                                  CVarGetInteger("gInterpolationFPS", 60));
    }

    return CVarGetInteger("gInterpolationFPS", 60);
}

uint32_t GameEngine::GetInterpolationFrameCount()
{
	return ceil((float)GetInterpolationFPS() / (60.0f / gVIsPerFrame));
}

extern "C" uint32_t GameEngine_GetInterpolationFrameCount() {
	return GameEngine::GetInterpolationFrameCount();
}

void GameEngine::ShowMessage(const char* title, const char* message, SDL_MessageBoxFlags type) {
#if defined(__SWITCH__)
    SPDLOG_ERROR(message);
#else
    SDL_ShowSimpleMessageBox(type, title, message, nullptr);
    SPDLOG_ERROR(message);
#endif
}

int GameEngine::ShowYesNoBox(const char* title, const char* box) {
    int ret;
#ifdef _WIN32
    ret = MessageBoxA(nullptr, box, title, MB_YESNO | MB_ICONQUESTION);
#elif defined(__SWITCH__)
    SPDLOG_ERROR(box);
    return IDYES;
#else
    SDL_MessageBoxData boxData = { 0 };
    SDL_MessageBoxButtonData buttons[2] = { { 0 } };

    buttons[0].buttonid = IDYES;
    buttons[0].text = "Yes";
    buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    buttons[1].buttonid = IDNO;
    buttons[1].text = "No";
    buttons[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    boxData.numbuttons = 2;
    boxData.flags = SDL_MESSAGEBOX_INFORMATION;
    boxData.message = box;
    boxData.title = title;
    boxData.buttons = buttons;
    SDL_ShowMessageBox(&boxData, &ret);
#endif
    return ret;
}

bool GameEngine::HasVersion(SF64Version ver){
    auto versions = Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager()->GetGameVersions();
    return std::find(versions.begin(), versions.end(), ver) != versions.end();
}

extern "C" bool GameEngine_HasVersion(SF64Version ver) {
    return GameEngine::HasVersion(ver);
}

extern "C" uint32_t GameEngine_GetSampleRate() {
    auto player = Ship::Context::GetInstance()->GetAudio()->GetAudioPlayer();
    if (player == nullptr) {
        return 0;
    }

    if (!player->IsInitialized()) {
        return 0;
    }

    return player->GetSampleRate();
}

extern "C" uint32_t GameEngine_GetSamplesPerFrame() {
    return SAMPLES_PER_FRAME;
}

// End

Fast::Interpreter* GameEngine_GetInterpreter() {
    return static_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow())
             ->GetInterpreterWeak()
             .lock()
             .get();
}

extern "C" float GameEngine_GetAspectRatio() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.aspect_ratio;
}

extern "C" uint32_t GameEngine_GetGameVersion() {
    return 0x00000001;
}

static const char* sOtrSignature = "__OTR__";

extern "C" uint8_t GameEngine_OTRSigCheck(const char* data) {
    if (data == nullptr) {
        return 0;
    }
    return strncmp(data, sOtrSignature, strlen(sOtrSignature)) == 0;
}

extern "C" void GameEngine_GetTextureInfo(const char* path, int32_t* width, int32_t* height, float* scale, bool* custom) {
    if(GameEngine_OTRSigCheck(path) != 1){
        *custom = false;
        return;
    }
    std::shared_ptr<Fast::Texture> tex = std::static_pointer_cast<Fast::Texture>(
        Ship::Context::GetInstance()->GetResourceManager()->LoadResourceProcess(path));
    *width = tex->Width;
    *height = tex->Height;
    *scale = tex->VPixelScale;
    *custom = tex->Flags & (1 << 0);
}

extern "C" float __cosf(float angle) throw() {
    return cosf(angle);
}

extern "C" float __sinf(float angle) throw() {
    return sinf(angle);
}

extern "C" float SIN_DEG(float angle) {
    return __sinf(M_DTOR * angle);
}
extern "C" float COS_DEG(float angle) {
    return __cosf(M_DTOR * angle);
}

struct TimedEntry {
    uint64_t duration;
    TimerAction action;
    int32_t* address;
    int32_t value;
    bool active;
};

std::vector<TimedEntry> gTimerTasks;

uint64_t Timer_GetCurrentMillis() {
    return SDL_GetTicks();
}

extern "C" s32 Timer_CreateTask(u64 time, TimerAction action, s32* address, s32 value) {
    const auto millis = Timer_GetCurrentMillis();
    TimedEntry entry = {
        .duration = millis + CYCLES_TO_MSEC_PC(time),
        .action = action,
        .address = address,
        .value = value,
        .active = true,
    };

    gTimerTasks.push_back(entry);

    return gTimerTasks.size() - 1;
}

extern "C" void Timer_Increment(int32_t* address, int32_t value) {
    *address += value;
}

extern "C" void Timer_SetValue(int32_t* address, int32_t value) {
    *address = value;
}

void Timer_CompleteTask(TimedEntry& task) {
    if (task.action != nullptr) {
        task.action(task.address, task.value);
    }
    task.active = false;
}

extern "C" void Timer_Update() {

    if (gTimerTasks.empty()) {
        return;
    }

    const auto millis = Timer_GetCurrentMillis();

    for (auto& task : gTimerTasks) {
        if (task.active && millis >= task.duration) {
            Timer_CompleteTask(task);
        }
    }
}

// Gets the width of the main ImGui window
extern "C" uint32_t OTRGetCurrentWidth() {
    return GameEngine::Instance->context->GetWindow()->GetWidth();
}

// Gets the height of the main ImGui window
extern "C" uint32_t OTRGetCurrentHeight() {
    return GameEngine::Instance->context->GetWindow()->GetHeight();
}

extern "C" float OTRGetHUDAspectRatio() {
    if (CVarGetInteger("gHUDAspectRatio.Enabled", 0) == 0 || CVarGetInteger("gHUDAspectRatio.X", 0) == 0 || CVarGetInteger("gHUDAspectRatio.Y", 0) == 0) {
        return GameEngine_GetAspectRatio();
    }
    return ((float)CVarGetInteger("gHUDAspectRatio.X", 1) / (float)CVarGetInteger("gHUDAspectRatio.Y", 1));
}

extern "C" float OTRGetDimensionFromLeftEdge(float v) {
    auto interpreter = GameEngine_GetInterpreter();
    return (interpreter->mNativeDimensions.width / 2 - interpreter->mNativeDimensions.height / 2 * interpreter->mCurDimensions.aspect_ratio + (v));
}

extern "C" float OTRGetDimensionFromRightEdge(float v) {
    auto interpreter = GameEngine_GetInterpreter();
    return (interpreter->mNativeDimensions.width / 2 + interpreter->mNativeDimensions.height / 2 * interpreter->mCurDimensions.aspect_ratio -
            (interpreter->mNativeDimensions.width - v));
}

extern "C" float OTRGetDimensionFromLeftEdgeForcedAspect(float v, float aspectRatio) {
    auto interpreter = GameEngine_GetInterpreter();
    return (interpreter->mNativeDimensions.width / 2 - interpreter->mNativeDimensions.height / 2 * (aspectRatio > 0 ? aspectRatio : interpreter->mCurDimensions.aspect_ratio) + (v));
}

extern "C" float OTRGetDimensionFromRightEdgeForcedAspect(float v, float aspectRatio) {
    auto interpreter = GameEngine_GetInterpreter();
    return (interpreter->mNativeDimensions.width / 2 + interpreter->mNativeDimensions.height / 2 * (aspectRatio > 0 ? aspectRatio : interpreter->mCurDimensions.aspect_ratio) -
            (interpreter->mNativeDimensions.width - v));
}

extern "C" float OTRGetDimensionFromLeftEdgeOverride(float v) {
    return OTRGetDimensionFromLeftEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

extern "C" float OTRGetDimensionFromRightEdgeOverride(float v) {
    return OTRGetDimensionFromRightEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

// Gets the width of the current render target area
extern "C" uint32_t OTRGetGameRenderWidth() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.width;
}

// Gets the height of the current render target area
extern "C" uint32_t OTRGetGameRenderHeight() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.height;
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdge(float v) {
    return ((int) floorf(OTRGetDimensionFromLeftEdge(v)));
}

extern "C" int16_t OTRGetRectDimensionFromRightEdge(float v) {
    return ((int) ceilf(OTRGetDimensionFromRightEdge(v)));
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdgeForcedAspect(float v, float aspectRatio) {
    return ((int) floorf(OTRGetDimensionFromLeftEdgeForcedAspect(v, aspectRatio)));
}

extern "C" int16_t OTRGetRectDimensionFromRightEdgeForcedAspect(float v, float aspectRatio) {
    return ((int) ceilf(OTRGetDimensionFromRightEdgeForcedAspect(v, aspectRatio)));
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdgeOverride(float v) {
    return OTRGetRectDimensionFromLeftEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

extern "C" int16_t OTRGetRectDimensionFromRightEdgeOverride(float v) {
    return OTRGetRectDimensionFromRightEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

extern "C" int32_t OTRConvertHUDXToScreenX(int32_t v) {
    auto interpreter = GameEngine_GetInterpreter();
    float gameAspectRatio = interpreter->mCurDimensions.aspect_ratio;
    int32_t gameHeight = interpreter->mCurDimensions.height;
    int32_t gameWidth = interpreter->mCurDimensions.width;
    float hudAspectRatio = 4.0f / 3.0f;
    int32_t hudHeight = gameHeight;
    int32_t hudWidth = hudHeight * hudAspectRatio;
    float hudScreenRatio = (hudWidth / 320.0f);
    float hudCoord = v * hudScreenRatio;
    float gameOffset = (gameWidth - hudWidth) / 2;
    float gameCoord = hudCoord + gameOffset;
    float gameScreenRatio = (320.0f / gameWidth);
    float screenScaledCoord = gameCoord * gameScreenRatio;
    int32_t screenScaledCoordInt = screenScaledCoord;
    return screenScaledCoordInt;
}

extern "C" void* GameEngine_Malloc(size_t size) {
    MemoryPool.push_back(new uint8_t[size]);
    return (void*) MemoryPool.back();
}

extern "C" void GameEngine_Free(void* ptr) {
    for (auto it = MemoryPool.begin(); it != MemoryPool.end(); ++it) {
        if (*it == ptr) {
            free(ptr);
            MemoryPool.erase(it);
            break;
        }
    }
}

/* i64 (i64_module.cpp): the audio thread loads a sequence and its sound bank synchronously when music first starts,
 * and the game thread waits for it (an ~50-110 ms hitch half a second into a run). Load all audio (~10 MB) on LUS's
 * worker threads at the start of a run instead. */
extern "C" void GameEngine_PreloadAudio(void) {
    if (auto rm = Ship::Context::GetInstance()->GetResourceManager()) {
        rm->LoadResourcesAsync("ast_audio/*");
    }
}
