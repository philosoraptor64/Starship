# Star Fox 64 as an i64 module: hand-off notes

State at Starship `9652654e` (branch `netplay`), LUS fork `starship-netplay` `d1370e10`, Torch fork `starship-netplay`
`e10b9c2` (2026-11-13). The netplay design itself (SfNet, rollback, Versus) is in `docs/netplay-notes.md`.

## Build
- Mac:
  `cmake -H. -Bbuild-module -GNinja -DCMAKE_BUILD_TYPE=Release -DSTARSHIP_I64_MODULE=ON && cmake --build build-module`
  -> `libsf64_i64.dylib` + `libsf64_i64.resources/` (config.yml, assets/yaml, starship.o2r).
  Install with `i64/scripts/install-module.sh build-module/libsf64_i64.dylib build-module/libsf64_i64.resources/*`.
- iOS:
  `cmake -S . -B build-ios-module -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DPLATFORM=OS64 -DDEPLOYMENT_TARGET=17.0 -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release -DSTARSHIP_I64_MODULE=ON -DLUS_SDL2_PREBUILT=$HOME/Development/N64/i64/deps-out/ios -DBUILD_SM64=OFF`
  (configure takes ~10 min). Ship the dylib plus the resources folder.
- `starship.o2r` (the port's own assets) must exist in the build dir or repo root (the GeneratePortO2R target).

## Module (src/port/i64_module.cpp)
- id `sf64-us`; accepts the ROM USA 1.1, SHA-1 09f0d105.
- Import: Torch Companion -> `<data>/sf64.o2r`. Recipe stamp `sf64.o2r.recipe` = `SF64_ASSET_RECIPE` (cmake/AssetRecipeId.cmake: config.yml + assets/yaml + Torch src); another stamp -> "import the ROM again".
- `SHIP_HOME` = i64's data folder (config, `default.sav`, logs). starship.o2r is copied there from the resources.
- Run split: `Game.cpp` Port_RunBegin / Port_RunFrame / Port_RunEnd(full). A full end deletes GameEngine (the only owner of the LUS context) and resets spdlog. Re-entry: `Reentry.cpp` restores `__DATA` + `__SFSNAP` (kept state lives in `__DATA,__sfkeep`).
- Audio: `gPortHostAudioWrite/Buffered` (Engine.cpp, stereo at 32 kHz); `LUS_SetExternalAudio` stops LUS from opening an audio device.
- Online: `SfNet_InitExternal` on i64's channel.
  - i64 never calls `n64_game_session_blob` yet, so with an empty `launch->session_blob` the module runs an **exchange phase** before starting the game.
  - In it the host sends `SfNet_SessionBlob` (520 bytes, packet 'SFBL' type 1 + run number) every 6 frames; the guest acks (type 2), and either side starts.
  - When i64 delivers blobs, the phase is skipped automatically.
- Pause: `SfNet_SetAway`. Peer left -> frame returns EXIT.
- Logs: i64.log `sf64: online as player N (session data 520 bytes)`; SfNet lines go to stderr (Mac) / `~/Documents/sfnet.log` (iOS).

## LUS fork changes (old upstream eaaf9d0f + ours)
- Ported by hand from the SpaghettiKart/BattleShip forks:
  - `LUS_SetExternalWindow/Pacing`;
  - Metal releasing destructor + drawable ownership;
  - `LUS_OBJC_PREFIX` (SF64);
  - `LUS_SHARED_SDL`/`LUS_SDL2_PREBUILT`;
  - SHIP_HOME on iOS;
  - the interpreter weak-instance fix.
- New: `LUS_SetExternalAudio`. The old SDLAudioPlayer opens the single legacy device, which failed endlessly inside i64.
- Static spdlog in module builds: global imported targets at the top level of CMakeLists.txt, plus switches in LUS (mac.cmake, src/CMakeLists.txt) and Torch (`TORCH_STATIC_SPDLOG`). fmt stays a shared Homebrew lib, like MK.
- `Snapshot.c` uses `__dso_handle` (works in a dylib).

## Results (Mac, Debug i64)
- Import, the opening cutscene, 3 runs in one process.
- Heap left per run 1.2 MB (`I64_TEST_HEAP_DIFF`; was 8.2 MB before samples were freed at stop: `SF64::gAudioSampleFreeData`).
- Online pair test: SfNet sync checks 31/30 matched, no desync.

## Open
- Phone tests (single player on the 14 Pro, then two phones).
- **Per-frame cost 12.6-14.5 ms of "game time per frame"** on the Studio, with gaps over 25 ms in about 2-4% of frames solo (most frames when two copies share the Mac). Not profiled. First suspects:
  - the present waiting for vsync inside the frame call (host-paced: the drawable wait counts as game time; check `mVsyncEnabled`/the Metal layer's displaySync with `LUS_SetExternalPacing`);
  - Starship's frame interpolation drawing extra frames;
  - the Debug i64 build.
  Measure on the phone first.
- The remaining ~1.2 MB per run is unattributed.
- `n64_game_set_paused` uses SfNet away; there's no in-game pause menu on resume (pause_ready returns 1 at once).
- Moving to the newer shared LUS (MK's fork) is a large separate job.
